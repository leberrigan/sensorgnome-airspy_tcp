/*
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2013 by Hoernchen <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Airspy port :
 * Copyright (C) 2018 by Thierry Leconte http://www.github.com/TLeconte
 *
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
	#include <time.h>
	#include <unistd.h>
	#include <arpa/inet.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <netinet/in.h>
	#include <fcntl.h>
	#include <sys/un.h>
#else
	#include <winsock2.h>
	#include "getopt/getopt.h"
#endif

#include <pthread.h>

#include <libairspy/airspy.h>

//typedef int socklen_t;

#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1
#define UNIX_PATH_MAX 108

static SOCKET s;
static int wait_for_start = 0;

static pthread_t tcp_worker_thread;
static pthread_t command_thread;
static pthread_cond_t exit_cond;
static pthread_mutex_t exit_cond_lock;

static pthread_mutex_t ll_mutex;
static pthread_cond_t cond;

static pthread_condattr_t cond_attr; // for setting CLOCK_MONOTONIC

struct llist {
	char *data;
	size_t len;
	struct llist *next;
};

#ifndef _WIN32
	/* sample stream header, for embedding in output stream for clients */
	typedef struct {
			uint32_t size;      // size of this header plus number of sample bytes before next header
			double ts;          // double timestamp of first sample in stream
	} stream_segment_hdr_t;
#endif

typedef struct { /* structure size must be multiple of 2 bytes */
	char magic[4];
	uint32_t tuner_type;
	uint32_t tuner_gain_count;
} dongle_info_t;

static struct airspy_device* dev = NULL;
static uint32_t samp_rate = 3000000;

static uint32_t fscount,*supported_samplerates;
static int verbose=0;
static int ppm_error=0;
static int dshift=1;

static int enable_biastee = 0;
static int global_numq = 0;
/* 
static struct llist *ls_buffer = NULL;
static struct llist *le_buffer = NULL;
static int llbuf_num = 64; */
static struct llist *ll_buffers = 0;
static int llbuf_num = 500;

static volatile int do_exit = 0;
static int usb_buffer_size = 0;

/* parameter cache; some airspy_set_XXX methods don't have a corresponding
   airspy_get_XXX, so we cache successful values here.
*/

static
uint32_t p_freq = 0,
		p_samp_rate = 0,
		p_gain = 0,
		p_agc = 0,
        p_lna_gain = 0,
		p_mixer_gain = 0,
		p_vga_gain = 0,
		p_tuner_gain = 0, 
		p_bias_tee,
        p_streaming = 0;


void usage(void)
{
	printf("airspy_tcp: A rtl-tcp compatible, I/Q server for airspy SDR on the SensorGnome\n\n"
/* 		"Usage:\t[-a listen address]\n"
		"\t[-p listen port (default: 1234)]\n"
		"\t[-f frequency to tune to [Hz]]\n"
		"\t[-g gain (default: 0 for auto)]\n"
		"\t[-s samplerate in Hz ]\n"
		"\t[-n max number of linked list buffer to keep ]\n"
		"\t[-T enable bias-T ]\n"
		"\t[-P ppm_error (default: 0) ]\n"
		"\t[-D g digital shift (default : 1) ]\n"
		"\t[-v Verbose ]\n"); */

	
		"Usage:\t[-a listen address]\n"
		"\t[-S device serial number]\n"									// Modified!
		"\t[-p listen port or unix domain socket path (default: 1234)]\n"									// Modified!
		"\t[-f frequency to tune to [Hz]]\n"
		"\t[-g gain (default: 0 for auto)]\n"
		"\t[-s samplerate in Hz (default: 3000000 Hz)]\n"
		
		"\t[-b number of buffers (default: 15, set by library)]\n"											// New!
		"\t[-B libusb buffer size, multiple of 512 (default: 16 * 32 * 512 bytes, set by library)]\n"		// New!
		"\t[-n max number of linked list buffers to keep (default: 500)]\n"
			
	//	"\t[-d device index (default: 0)]\n"																// New!
    //            "\t[-t test mode: send RTL2832 internal counter, not real samples]\n"						// New!
			
		"\t[-T enable bias-T on GPIO PIN 0 (works for rtl-sdr.com v3 dongles)]\n"
                "\t[-P ppm_error (default: 0)]\n"
	
		"\t[-D g digital shift (default : 1) ]\n"
		"\t[-v Verbose ]\n");
	exit(1);
}

static void sighandler(int signum)
{
	do_exit = 1;
	pthread_cond_signal(&cond);
}

static int rx_callback(airspy_transfer_t* transfer)
{
	
	struct timespec ts;
	if(!do_exit && ! wait_for_start) {
			
		unsigned char *buf;
		uint32_t len;
	
		len=2*transfer->sample_count;
		buf=(short *)transfer->samples;
		
        char *dest;
		struct llist *rpt = (struct llist*)malloc(sizeof(struct llist));
	    uint32_t needlen;
		
		#ifndef _WIN32
			stream_segment_hdr_t *hdr;
			needlen = len + sizeof(stream_segment_hdr_t);
		#else
			needlen = len;
		#endif
	
		rpt->data = (char*)malloc(needlen);
		dest = rpt->data;
		
		#ifndef _WIN32
			/* fill in stream_segment_hdr_t and set dest to point after it  */
			hdr = (stream_segment_hdr_t *) rpt->data;
			clock_gettime(CLOCK_REALTIME, &ts);
			/* set start-of-buffer timestamp to current clock minus (# frames) * rate */
			hdr->ts = ts.tv_sec + ts.tv_nsec / 1.0e9 - (len / 2.0) / samp_rate;
			hdr->size = len + sizeof(stream_segment_hdr_t);
			dest += sizeof(stream_segment_hdr_t);
		#endif
	
		/* int i;
		char *data; */
	
		memcpy(dest, buf, len);

		rpt->len = needlen;
		rpt->next = NULL;

		/* data=rpt->data; */
	
		/* for(i=0;i<len;i++,buf++,data++) {
			short v=*buf<<dshift;
			short o;

			 // stupid added offset, because osmosdr client code 
			 // try to compensate rtl dongle offset 
			 o=(v-154)>>8;

			// round to 8bits half up to even
			if(v&0x80) {
			 if(v&0x7f) {o++;} else { if(v&0x100) o++;}
			}

			*data=(unsigned char)((o&0xff)+128);
		} */

		pthread_mutex_lock(&ll_mutex);

		/* if (ls_buffer == NULL)
		{
			ls_buffer = le_buffer = rpt;
		}
		else
		{
			le_buffer->next = rpt;
			le_buffer = rpt;
		}
		global_numq++;

		if(global_numq>llbuf_num) {
			struct llist *curelem;
			curelem=ls_buffer;
			ls_buffer=ls_buffer->next;
			if(ls_buffer==NULL) le_buffer=NULL;
			global_numq--;
			free(curelem->data);
			free(curelem);
		} */

		if (ll_buffers == NULL) {
			ll_buffers = rpt;
		} else {
			struct llist *cur = ll_buffers;
			int num_queued = 0;

			while (cur->next != NULL) {
				cur = cur->next;
				num_queued++;
			}

			if(llbuf_num && llbuf_num == num_queued-2){
				struct llist *curelem;

				free(ll_buffers->data);
				curelem = ll_buffers->next;
				free(ll_buffers);
				ll_buffers = curelem;
			}

			cur->next = rpt;
			global_numq = num_queued;
		}

		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&ll_mutex);
		
	}
	return 0;
}

static void *tcp_worker(void *arg)
{
	/* struct llist *curelem;
	int bytesleft,bytessent, index; */

	struct llist *curelem,*prev;
	int bytesleft,bytessent, index;
	struct timeval tv= {1,0};
	struct timespec ts;
	fd_set writefds;
	int r = 0;

	while(1) {

/* 		pthread_mutex_lock(&ll_mutex);
		while(ls_buffer==NULL && do_exit==0)
			pthread_cond_wait(&cond, &ll_mutex);

		if(do_exit) {
			pthread_mutex_unlock(&ll_mutex);
			pthread_exit(0);
		} */
	
		if(do_exit)
			pthread_exit(0);

		pthread_mutex_lock(&ll_mutex);
		clock_gettime(CLOCK_MONOTONIC, &ts);
                ts.tv_sec += 5; // timeout in 5 seconds
		r = pthread_cond_timedwait(&cond, &ll_mutex, &ts);
		if(r == ETIMEDOUT && ! wait_for_start) {
			pthread_mutex_unlock(&ll_mutex);
			fprintf(stderr, "worker cond timeout\n");
                        fflush(stderr);
			do_exit = 1;
			pthread_exit(NULL);
		}

		/* curelem = ls_buffer;
		ls_buffer=ls_buffer->next;
		global_numq--; */
		curelem = ll_buffers;
		ll_buffers = 0;
		pthread_mutex_unlock(&ll_mutex);

		while(curelem != 0) {
			bytesleft = curelem->len;
			index = 0;
			bytessent = 0;
			while(bytesleft > 0) {
				FD_ZERO(&writefds);
				FD_SET(s[1], &writefds);
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				r = select(s[1]+1, NULL, &writefds, NULL, &tv);
				if(r > 0 && ! do_exit) {
					bytessent = send(s[1],	&curelem->data[index], bytesleft, 0);
					bytesleft -= bytessent;
					index += bytessent;
				}
				if(r < 0 || bytessent == SOCKET_ERROR || do_exit) {
					fprintf(stderr, "worker socket bye\n");
                                        fflush(stderr);
					do_exit = 1;
					pthread_exit(NULL);
				}
			}
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}
	/* 	bytesleft = curelem->len;
		index = 0;
		while(bytesleft > 0) {
			bytessent = send(s,  &curelem->data[index], bytesleft, 0);
			bytesleft -= bytessent;
			index += bytessent;
			if(bytessent == SOCKET_ERROR || do_exit) {
					printf("worker socket bye\n");
					sighandler(0);
					pthread_exit(NULL);
			}
		}
		free(curelem->data);
		free(curelem); */
	}
}

struct command{
	unsigned char cmd;
	unsigned int param;
}__attribute__((packed));


static int set_agc(uint8_t value)
{
	int r;

	r=airspy_set_lna_agc(dev, value);
        if( r != AIRSPY_SUCCESS ) return r;


	r=airspy_set_mixer_agc(dev, value);
        return r;
}

static int set_samplerate(uint32_t fs)
{
	int r,i;

        for(i=0;i<fscount;i++)
      		if(supported_samplerates[i]==fs) break;
	if(i>=fscount) {
		printf("sample rate %d not supported\n",fs);
		return AIRSPY_ERROR_INVALID_PARAM;
	}

       	r=airspy_set_samplerate(dev, i);
	return r;
}

static int set_freq(uint32_t f)
{
	int r;

    r=airspy_set_freq(dev, (uint32_t)((float)f*(1.0+(float)ppm_error/1e6)));
	
	return r;
}

static void *command_worker(void *arg)
{
	int left, received = 0;
	fd_set readfds;
	struct command cmd={0, 0};
	struct timeval tv= {1, 0};
	int r = 0;
	uint32_t tmp;
	
	#define REPLY_BUFF_SIZE 1400
	char rbuf[REPLY_BUFF_SIZE + 1];

	while(1) {
		left=sizeof(cmd);
		while(left >0) {
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(s+1, &readfds, NULL, NULL, &tv);
			if(r) {
				received = recv(s, (char*)&cmd+(sizeof(cmd)-left), left, 0);
				left -= received;
			}
			if(received == SOCKET_ERROR || do_exit) {
				printf("comm recv bye\n");
				sighandler(0);
				pthread_exit(NULL);
			}
		}
		switch(cmd.cmd) {
		case 0x01:
			if(verbose) printf("set freq: %d\n", ntohl(cmd.param));
			set_freq(ntohl(cmd.param));
			p_freq = (int)ntohl(cmd.param);
			break;
		case 0x02:
			if(verbose) printf("set sample rate: %d\n", ntohl(cmd.param));
			set_samplerate(ntohl(cmd.param));
			p_samp_rate = (int)ntohl(cmd.param);
			break;
		case 0x03:
			if(verbose) printf("set gain: %d\n", ntohl(cmd.param));
			airspy_set_linearity_gain(dev,(ntohl(cmd.param)+250)/37);
			p_gain = (int)ntohl(cmd.param);
			break;
		case 0x04:
			if(verbose) printf("set agc: %d\n", ntohl(cmd.param));
			set_agc(ntohl(cmd.param));
			p_agc = (int)ntohl(cmd.param);
			break;
		case 0x05:
			if(verbose) printf("set lna gain: %d\n",ntohl(cmd.param));
			airspy_set_lna_gain(dev, ntohl(cmd.param));
			p_lna_gain = (int)ntohl(cmd.param);
			break;
		case 0x06:
			if(verbose) printf("set mixer gain: %d\n",ntohl(cmd.param));
			airspy_set_mixer_gain(dev, ntohl(cmd.param));
			p_mixer_gain = (int)ntohl(cmd.param);
			break;
		case 0x07:
			if(verbose) printf("set vga gain: %d\n",ntohl(cmd.param));
			airspy_set_vga_gain(dev, ntohl(cmd.param));
			p_vga_gain = (int)ntohl(cmd.param);
			break;
		case 0x08:
			if(verbose) printf("set tuner gain: %d \n", ntohl(cmd.param));
			airspy_set_linearity_gain(dev,ntohl(cmd.param));
			p_tuner_gain = (int)ntohl(cmd.param);
			break;
		case 0x09:
			if(verbose) printf("set bias tee: %d\n", ntohl(cmd.param));
			airspy_set_rf_bias(dev, (int)ntohl(cmd.param));
			p_bias_tee = (int)ntohl(cmd.param);
			break;
		case 0x60:
			if (cmd.param) {
					fprintf(stderr, "start streaming i/q samples\n");
					fflush(stderr);
					wait_for_start = 0;
					p_streaming = 1;
			} else {
					fprintf(stderr, "stop streaming i/q samples\n");
					fflush(stderr);
					wait_for_start = 1;
					p_streaming = 0;
			}
			break;
		default:
			printf("Unknown command %d param %d\n", cmd.cmd, ntohl(cmd.param));
			break;
		}



		sprintf(rbuf, "{"
				"\"frequency\": %d,"
				"\"rate\": %d,"
				"\"gain\": %d,"
				"\"agc\": %d,"
				"\"lna_gain\": %d,"
				"\"mixer_gain\": %d,"
				"\"vga_gain\": %d,"
				"\"tuner_gain\": %d,"
				"\"bias_tee\": %d,"
				"\"streaming\": %d"
				"}\n",
				p_freq,
				p_samp_rate,
				p_gain,
				p_agc,
				p_lna_gain,
				p_mixer_gain,
				p_vga_gain,
				p_tuner_gain,
				p_bias_tee,
				p_streaming);

		send(s, rbuf, strlen(rbuf), 0);

		cmd.cmd = 0xff;
	}
}

int main(int argc, char **argv)
{
	int r, opt;
	char* addr = "127.0.0.1";
	uint64_t serno = 0;
	int port = 1234;
	uint32_t frequency = 100000000;
	struct sockaddr_in local, remote;
	uint32_t buf_num = 0;
	int dev_index = 0;
	int dev_given = 0;
	int gain = 0;
	struct llist *curelem,*prev;
	pthread_attr_t attr;
	void *status;
	struct timeval tv = {1,0};
	struct linger ling = {1,0};
	SOCKET listensocket;
	socklen_t rlen;
	fd_set readfds;
	dongle_info_t dongle_info;
	struct sigaction sigact, sigign;
	char sock_path[1 + UNIX_PATH_MAX];
	struct sockaddr_un local_u;
	char *tmp;
	int use_unix_sock = 0;

	//int num_cons;
	while ((opt = getopt(argc, argv, "S:a:p:f:g:s:b:B:n:P:TD:v")) != -1) {
		switch (opt) {
		case 'S':
			serno = strtoull(optarg, NULL, 16);
			break;
		case 'f':
			frequency = (uint32_t)atoi(optarg);
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10); /* tenths of a dB */
			break;
		case 's':
			samp_rate = (uint32_t)atoi(optarg);
			break;
		case 'a':
			addr = optarg;
			break;
		case 'p':
#ifndef _WIN32
			port = (int)strtol(optarg, &tmp, 0);
			if (*tmp != '\0') {
				strncpy(sock_path, optarg, UNIX_PATH_MAX);
				use_unix_sock = 1;
			}
#else
			port = atoi(optarg);
#endif
			break;
		case 'T':
			enable_biastee = 1;
			break;
                case 'P':
                        ppm_error = atoi(optarg);
                        break;
                case 'D':
                        dshift = atoi(optarg);
                        break;
		case 'v':
			verbose = 1;
			break;
		case 'b':
			buf_num = atoi(optarg);
			break;
		case 'B':
			usb_buffer_size = atoi(optarg);
			break;
		case 'n':
			llbuf_num = atoi(optarg);
			break;

		default:
			usage();
			break;
		}
	}

	if (argc < optind)
		usage();

	if (serno == 0) {
		exit(2);
	}

	r = airspy_open_sn(&dev, serno);
	if( r != AIRSPY_SUCCESS ) {
			fprintf(stderr,"airspy_open() failed: %s (%d)\n", airspy_error_name(r), r);
			airspy_exit();
			return -1;
	}


	r = airspy_set_sample_type(dev, AIRSPY_SAMPLE_INT16_IQ);
	if( r != AIRSPY_SUCCESS ) {
			fprintf(stderr,"airspy_set_sample_type() failed: %s (%d)\n", airspy_error_name(r), r);
			airspy_close(dev);
			airspy_exit();
			return -1;
	}

	airspy_set_packing(dev, 1);

	r=airspy_get_samplerates(dev, &fscount, 0);
	if( r != AIRSPY_SUCCESS ) {
			fprintf(stderr,"airspy_get_sample_rate() failed: %s (%d)\n", airspy_error_name(r), r);
			airspy_close(dev);
			airspy_exit();
			return -1;
	}
	supported_samplerates = (uint32_t *) malloc(fscount * sizeof(uint32_t));
	r=airspy_get_samplerates(dev, supported_samplerates, fscount);
	if( r != AIRSPY_SUCCESS ) {
			fprintf(stderr,"airspy_get_sample_rate() failed: %s (%d)\n", airspy_error_name(r), r);
			airspy_close(dev);
			airspy_exit();
			return -1;
	}

	if(samp_rate) {
        	r = set_samplerate(samp_rate);
        	if( r != AIRSPY_SUCCESS ) {
                	fprintf(stderr,"set_samplerate() failed: %s (%d)\n", airspy_error_name(r), r);
                	airspy_close(dev);
                	airspy_exit();
                	return -1;
        	}
	} else {
       		r=airspy_set_samplerate(dev, fscount-1);
        	if( r != AIRSPY_SUCCESS ) {
                	fprintf(stderr,"airspy_set_samplerate() failed: %s (%d)\n", airspy_error_name(r), r);
                	airspy_close(dev);
                	airspy_exit();
                	return -1;
        	}
	}

	/* Set the frequency */
	r = set_freq(frequency);
	if( r != AIRSPY_SUCCESS ) {
			fprintf(stderr,"airspy_set_freq() failed: %s (%d)\n", airspy_error_name(r), r);
			airspy_close(dev);
			airspy_exit();
			return -1;
	}

	if (0 == gain) {
	 /* Enable automatic gain */
		r=set_agc(1);
		if( r != AIRSPY_SUCCESS ) {
				fprintf(stderr,"airspy_set agc failed: %s (%d)\n", airspy_error_name(r), r);
		}
	} else {
        	r = airspy_set_linearity_gain(dev,(gain+250)/37);
       		if( r != AIRSPY_SUCCESS ) {
               		fprintf(stderr,"set gains failed: %s (%d)\n", airspy_error_name(r), r);
               		airspy_close(dev);
               		airspy_exit();
               		return -1;
       		}
		if(verbose) fprintf(stderr, "Tuner gain set to %f dB.\n", gain/10.0);
	}

	r = airspy_set_rf_bias(dev, enable_biastee);
	if( r != AIRSPY_SUCCESS ) {
			fprintf(stderr,"airspy_set_rf_bias() failed: %s (%d)\n", airspy_error_name(r), r);
			airspy_close(dev);
			airspy_exit();
			return -1;
	}

	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);

	pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_mutex_init(&ll_mutex, NULL);
	pthread_mutex_init(&exit_cond_lock, NULL);

	pthread_condattr_init(&cond_attr);
	pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);

	pthread_cond_init(&cond, NULL);
	pthread_cond_init(&exit_cond, NULL);
/*
	memset(&local,0,sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = inet_addr(addr);

	listensocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	r = 1;
	setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
	setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
	bind(listensocket,(struct sockaddr *)&local,sizeof(local));

	r = fcntl(listensocket, F_GETFL, 0);
	r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);
	
*/



#ifndef _WIN32
	if (use_unix_sock) {
		listensocket = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (listensocket < 0) {
			fprintf(stderr, "Error opening unix domain socket.\n");
			fflush(stderr);
			exit(4);
		}
	} else {
#endif
		memset(&local,0,sizeof(local));
		local.sin_family = AF_INET;
		local.sin_port = htons(port);
		local.sin_addr.s_addr = inet_addr(addr);

		listensocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifndef _WIN32
	}
#else
	r = 1;
	setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
	setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
#endif
#ifndef _WIN32
	if (use_unix_sock) {
		memset( (char *) &local_u, 0, sizeof(local_u));
		local_u.sun_family = AF_UNIX;
		strncpy(local_u.sun_path, sock_path, sizeof(local_u.sun_path) - 1);
		if (bind(listensocket, (struct sockaddr *) &local_u, sizeof(local_u)) < 0) {
			fprintf(stderr, "Error binding to unix domain socket at %s\n", sock_path);
						fflush(stderr);
			exit(5);
		}
	} else {
#endif
		bind(listensocket,(struct sockaddr *)&local,sizeof(local));
#ifdef _WIN32
		ioctlsocket(listensocket, FIONBIO, &blockmode);
#else
		r = fcntl(listensocket, F_GETFL, 0);
		r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);
	}
#endif

	listen(listensocket,1);

#ifndef _WIN32
	if (use_unix_sock) {
		printf("Listening on unix domain socket %s\n", sock_path);
				fflush(stdout);
	} else {
#endif
		printf("listening...\nUse the device argument 'rtl_tcp=%s:%d' in OsmoSDR "
			"(gr-osmosdr) source\n"
			"to receive samples in GRC and control "
			"rtl_tcp parameters (frequency, gain, ...).\n",
			addr, port);
#ifndef _WIN32
	}
#endif

	while(1) {
	//	num_cons = 0;
		while(1) {
			FD_ZERO(&readfds);
			FD_SET(listensocket, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(listensocket+1, &readfds, NULL, NULL, &tv);
			if(do_exit) {
				goto out;
			} else if(r>0) {
				rlen = sizeof(remote);
				s = accept(listensocket,(struct sockaddr *)&remote, &rlen);
				break;
			}
		}

		setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
		r=5;setsockopt(s, SOL_SOCKET, SO_PRIORITY, (char *)&r, sizeof(int));

		printf("client accepted!\n");

		memset(&dongle_info, 0, sizeof(dongle_info));
		memcpy(&dongle_info.magic, "RTL0", 4);

		dongle_info.tuner_type = htonl(5);
		dongle_info.tuner_gain_count = htonl(22);

		r = send(s, (const char *)&dongle_info, sizeof(dongle_info), 0);
		if (sizeof(dongle_info) != r) {
			printf("failed to send dongle information\n");
			break;
		}

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
		r = pthread_create(&tcp_worker_thread, &attr, tcp_worker, NULL);
		r = pthread_create(&command_thread, &attr, command_worker, NULL);
		pthread_attr_destroy(&attr);

		fprintf(stderr,"start rx\n");
		r = airspy_start_rx(dev, rx_callback, NULL);
		if( r != AIRSPY_SUCCESS ) {
        	fprintf(stderr,"airspy_start_rx() failed: %s (%d)\n", airspy_error_name(r), r);
			break;
		}

		pthread_join(tcp_worker_thread, &status);
		pthread_join(command_thread, &status);

		close(s);

		fprintf(stderr,"stop rx\n");

		r = airspy_stop_rx(dev);
		if( r != AIRSPY_SUCCESS ) {
			fprintf(stderr,"airspy_stop_rx() failed: %s (%d)\n", airspy_error_name(r), r);
			break;
		}

		curelem = ls_buffer;
		while(curelem != 0) {
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}
		ls_buffer=le_buffer=NULL;
		global_numq = 0;

		do_exit = 0;
	}

out:
	airspy_close(dev);
	airspy_exit();
	close(listensocket);
	close(s);
	printf("bye!\n");
	return r >= 0 ? r : -r;
}
