#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "qisr.h"
#include "msp_cmn.h"
#include "msp_errors.h"



#define FRAME_LEN   640       //20ms, 16K, 16bit
#define FIFO_LEN    1920000   //60s, 32*1000*60

#define BUFFER_SIZE 4096      //store result
#define HINTS_SIZE  100



//fifo to store pcm
char pcm_fifo[FIFO_LEN] = {0};
int cur_pos_w = 0;
int cur_pos_r = 0;
int left_num = 0;

//child process to arecord
pid_t arec_pid;



/* Thread to read wav continously and write to fifo */
void* thread_read_wav(void *arg)
{
	FILE* f_pcm = NULL;
	int n = 0;
	char tmp[FRAME_LEN] = {0};


	f_pcm = fopen("record.wav", "rb");
	if(f_pcm == NULL)
	{
		printf("wav not exist\n");
		return NULL;
	}
	
	usleep(200*1000);   //why 20ms not work?		
	n = fread((void*)tmp, 1, FRAME_LEN, f_pcm);
    printf("read %d\n",n);
	while(n)
	{
		memcpy((void*)(pcm_fifo+cur_pos_w), (void*)tmp, n);
		cur_pos_w = (cur_pos_w+n)%FIFO_LEN;
		left_num += n;
		
		usleep(20*1000);		
		n = fread((void*)tmp, 1, FRAME_LEN, f_pcm);
	}

	if(feof(f_pcm))
		printf("end of file, %ld\n", time(NULL));
	else
		printf("read file error:%d\n",ferror(f_pcm));

	//printf("close file, pointer now: %ld\n", ftell(f_pcm));
	fclose(f_pcm);

	return NULL;
}

void handle_iat_end()
{

	printf("iat end, %ld\n", time(NULL));
}

/* speech end, stop arecord */
void handle_speech_ending()
{
	int ret;

	printf("speech ending, %ld\n", time(NULL));

	if((ret = kill(arec_pid, SIGKILL)) == 0)
	{
		printf("kill arecord\n");
	}
}

/* Thread to run iat */
void* thread_run_iat(void *arg)
{
	int ret = MSP_SUCCESS;
	const char* login_params = "appid = 56e02c53, work_dir = .";
	const char* session_begin_params = "sub = iat, domain = iat, language = zh_cn, accent = mandarin, sample_rate = 16000, result_type = plain, result_encoding = utf8";
	const char* session_id = NULL;
	char rec_result[BUFFER_SIZE] = {NULL};
	char hints[HINTS_SIZE] = {NULL};
	unsigned int iat_len = 10*FRAME_LEN; //200ms
	unsigned int total_len = 0;
	int aud_stat = MSP_AUDIO_SAMPLE_FIRST;
	int ep_stat = MSP_EP_LOOKING_FOR_SPEECH;
	int rec_stat = MSP_REC_STATUS_SUCCESS;
	int errcode = MSP_SUCCESS;


	ret = MSPLogin(NULL, NULL, login_params);
	if(MSP_SUCCESS != ret)
	{
		printf("MSPLogin failed, Error code %d.\n", ret);
		goto exit;

	}

	printf("begin iat...\n");

	session_id = QISRSessionBegin(NULL, session_begin_params, &errcode);
	if(MSP_SUCCESS != errcode)
	{
		printf("QISRSessionBegin failed! error code:%d\n", errcode);
		goto exit;
	}


	while(1)
	{
        if(left_num >= iat_len)
        {
		    ret = QISRAudioWrite(session_id, (const void *)&pcm_fifo[cur_pos_r], iat_len, aud_stat, &ep_stat, &rec_stat);
		    if(MSP_SUCCESS != ret)
		    {
			    printf("QISRAudioWrite failed! error code:%d\n", ret);
			    goto exit;
		    }

		    //printf("rec_stat: %d\n", rec_stat);  //2, ISR_REC_STATUS_INCOMPLETE
		    if(MSP_REC_STATUS_SUCCESS == rec_stat)
		    {
			    const char *rslt = QISRGetResult(session_id, &rec_stat, 0, &errcode);
			    printf("get...%s\n", rslt);
			    if(MSP_SUCCESS != errcode)
			    {
				    printf("QISRGetResult failed! error code:%d\n", errcode);
				    goto exit;
			    }
			    if(NULL != rslt)
			    {
				    unsigned int rslt_len = strlen(rslt);
				    total_len += rslt_len;
				    if(total_len >= BUFFER_SIZE)
				    {
					    printf("no enough buffer for rec_result !\n");
					    goto exit;
				    }
				    strncat(rec_result, rslt, rslt_len);
			    }

		    }

		    cur_pos_r = (cur_pos_r+iat_len)%FIFO_LEN;	
		    left_num -= iat_len;
	
		    if(MSP_AUDIO_SAMPLE_FIRST == aud_stat)
			    aud_stat = MSP_AUDIO_SAMPLE_CONTINUE;

		    if(MSP_EP_AFTER_SPEECH == ep_stat)
			    break;
		
        }
        else
        {
            //wait 200ms
            usleep(200*1000);
        }
	}

	//case speech ending
	handle_speech_ending();

	errcode = QISRAudioWrite(session_id, NULL, 0, MSP_AUDIO_SAMPLE_LAST, &ep_stat, &rec_stat);
	if(MSP_SUCCESS != errcode)
	{
		printf("QISRAudioWrite failed! error code:%d\n", errcode);
		goto exit;
	}
	
	while(MSP_REC_STATUS_COMPLETE != rec_stat)
	{
		const char *rslt = QISRGetResult(session_id, &rec_stat, 0, &errcode);
		if(MSP_SUCCESS != errcode)
		{
			printf("QISRGetResult failed, error code:%d\n", errcode);
			goto exit;
		}
		if(NULL != rslt)
		{
			unsigned int rslt_len = strlen(rslt);
			total_len += rslt_len;
			if(total_len >= BUFFER_SIZE)
			{
				printf("no enough buffer for rec_result!\n");
				goto exit;
			}
			strncat(rec_result, rslt, rslt_len);
			printf("get...%s\n", rslt);
		}

		usleep(10*1000);

	}
	
	//iat ending
	handle_iat_end();
	printf("result: %s\n", rec_result);

exit:
	QISRSessionEnd(session_id, hints);
	MSPLogout();

	return NULL;
}


int main(int argc, char *argv[])
{
	FILE *f_pcm;
	pthread_t pth_wav, pth_iat; 

    //delete file first
    unlink("record.wav");

	if((arec_pid = fork()) == 0)
	{
		if(execl("/usr/bin/arecord", "arecord", "-D", "hw:0", "-r", "16000", "-c", "1", "-f", "S16_LE", "record.wav", NULL) < 0)
			perror("error on execl\n");
	}
	else
	{
		while(1)
		{
			f_pcm = fopen("record.wav", "rb");
			if(f_pcm != NULL)
				break;
			usleep(20*1000);    //need about 240ms
		}
	    fclose(f_pcm);
		pthread_create(&pth_wav, NULL, thread_read_wav, NULL);

		usleep(300*1000);      //a bit more than 200ms
		pthread_create(&pth_iat, NULL, thread_run_iat, NULL);

	
		pthread_join(pth_wav, NULL);
		pthread_join(pth_iat, NULL);

	}

	return 0;
}
