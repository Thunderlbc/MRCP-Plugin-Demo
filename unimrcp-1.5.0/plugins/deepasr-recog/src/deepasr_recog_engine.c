/*
 * Copyright 2008-2015 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */
#include <stdlib.h>
#include <stdbool.h>
#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include "DeepASRLib.h"


#define RECOG_ENGINE_TASK_NAME "DeepASR RECOGNITION Engine"

typedef struct deepasr_recog_engine_t deepasr_recog_engine_t;
typedef struct deepasr_recog_channel_t deepasr_recog_channel_t;
typedef struct deepasr_recog_msg_t deepasr_recog_msg_t;

/** Declaration of recognizer engine methods */
static apt_bool_t deepasr_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t deepasr_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t deepasr_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* deepasr_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	deepasr_recog_engine_destroy,
	deepasr_recog_engine_open,
	deepasr_recog_engine_close,
	deepasr_recog_engine_channel_create
};


/** Declaration of recognizer channel methods */
static apt_bool_t deepasr_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t deepasr_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t deepasr_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t deepasr_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	deepasr_recog_channel_destroy,
	deepasr_recog_channel_open,
	deepasr_recog_channel_close,
	deepasr_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t deepasr_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t deepasr_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t deepasr_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t deepasr_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);
static apt_bool_t deepasr_recog_stream_recog(deepasr_recog_channel_t *recog_channel,
							   const void *voice_data,
							   unsigned int voice_len 
							   ); 

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	deepasr_recog_stream_destroy,
	NULL,
	NULL,
	NULL,
	deepasr_recog_stream_open,
	deepasr_recog_stream_close,
	deepasr_recog_stream_write,
	NULL
};

/** Declaration of deepasr recognizer engine */
struct deepasr_recog_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of deepasr recognizer channel */
struct deepasr_recog_channel_t {
	/** Back pointer to engine */
	deepasr_recog_engine_t     *deepasr_engine;
	/** Engine channel base */
	mrcp_engine_channel_t   *channel;

	/** Active (in-progress) recognition request */
	mrcp_message_t          *recog_request;
	/** Pending stop response */
	mrcp_message_t          *stop_response;
	/** Indicates whether input timers are started */
	apt_bool_t               timers_started;
	/** Voice activity detector */
	mpf_activity_detector_t *detector;
	/** File to write utterance to */
	FILE                    *audio_out;
	
	const char				*session_id;
	const char				*last_result;
	apt_bool_t				recog_started;
};

typedef enum {
	DEEPASR_RECOG_MSG_OPEN_CHANNEL,
	DEEPASR_RECOG_MSG_CLOSE_CHANNEL,
	DEEPASR_RECOG_MSG_REQUEST_PROCESS
} deepasr_recog_msg_type_e;

/** Declaration of deepasr recognizer task message */
struct deepasr_recog_msg_t {
	deepasr_recog_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};

static apt_bool_t deepasr_recog_msg_signal(deepasr_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t deepasr_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN,"RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK   APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

/** Create deepasr recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	deepasr_recog_engine_t *deepasr_engine = apr_palloc(pool,sizeof(deepasr_recog_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	//if(!deepasr_login()) {
	//	return NULL;
	//}

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(deepasr_recog_msg_t),pool);
	deepasr_engine->task = apt_consumer_task_create(deepasr_engine,msg_pool,pool);
	if(!deepasr_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(deepasr_engine->task);
	apt_task_name_set(task,RECOG_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = deepasr_recog_msg_process;
	}

	/* create engine base */
	return mrcp_engine_create(
				MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
				deepasr_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t deepasr_recog_engine_destroy(mrcp_engine_t *engine)
{
	deepasr_recog_engine_t *deepasr_engine = engine->obj;
	if(deepasr_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(deepasr_engine->task);
		apt_task_destroy(task);
		deepasr_engine->task = NULL;
	}
	return TRUE;
}

/** Open recognizer engine */
static apt_bool_t deepasr_recog_engine_open(mrcp_engine_t *engine)
{
	deepasr_recog_engine_t *deepasr_engine = engine->obj;
	if(deepasr_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(deepasr_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close recognizer engine */
static apt_bool_t deepasr_recog_engine_close(mrcp_engine_t *engine)
{
	deepasr_recog_engine_t *deepasr_engine = engine->obj;
	if(deepasr_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(deepasr_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t* deepasr_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create deepasr recog channel */
	deepasr_recog_channel_t *recog_channel = apr_palloc(pool,sizeof(deepasr_recog_channel_t));
	recog_channel->deepasr_engine = engine->obj;
	recog_channel->recog_request = NULL;
	recog_channel->stop_response = NULL;
	recog_channel->detector = mpf_activity_detector_create(pool);
	recog_channel->audio_out = NULL;
	recog_channel->session_id = NULL;
	recog_channel->last_result = NULL;
	recog_channel->recog_started = FALSE;
	capabilities = mpf_sink_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			recog_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	recog_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			recog_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t deepasr_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destrtoy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t deepasr_recog_channel_open(mrcp_engine_channel_t *channel)
{	
	return deepasr_recog_msg_signal(DEEPASR_RECOG_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t deepasr_recog_channel_close(mrcp_engine_channel_t *channel)
{
	return deepasr_recog_msg_signal(DEEPASR_RECOG_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t deepasr_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return deepasr_recog_msg_signal(DEEPASR_RECOG_MSG_REQUEST_PROCESS,channel,request);
}

/** Process RECOGNIZE request */
static apt_bool_t deepasr_recog_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process RECOGNIZE request */
	mrcp_recog_header_t *recog_header;
	deepasr_recog_channel_t *recog_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	recog_channel->timers_started = TRUE;

	/* get recognizer header */
	recog_header = mrcp_resource_header_get(request);
	if(recog_header) {
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE) {
			recog_channel->timers_started = recog_header->start_input_timers;
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE) {
			mpf_activity_detector_noinput_timeout_set(recog_channel->detector,recog_header->no_input_timeout);
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
			mpf_activity_detector_silence_timeout_set(recog_channel->detector,recog_header->speech_complete_timeout);
		}
	}

	if(!recog_channel->audio_out) {
		const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
		char *file_name = apr_psprintf(channel->pool,"utter-%dkHz-%s.pcm",
							descriptor->sampling_rate/1000,
							request->channel_id.session_id.buf);
		char *file_path = apt_vardir_filepath_get(dir_layout,file_name,channel->pool);
		if(file_path) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Open Utterance Output File [%s] for Writing",file_path);
			recog_channel->audio_out = fopen(file_path,"wb");
			if(!recog_channel->audio_out) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Open Utterance Output File [%s] for Writing",file_path);
			}
		}
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);

	/* reset */
	int errcode;
	//const char*	session_begin_params = "sub = iat, domain = iat, language = zh_cn, accent = mandarin, sample_rate = 8000, result_type = plain, result_encoding = utf8";
	recog_channel->session_id = begin_session("INSURANCE", "audio/x-raw, layout=(string)interleaved, rate=(int)16000, format=(string)S16LE, channels=(int)1", &errcode); //听写不需要语法，第一个参数为NULL
	//recog_channel->session_id = QISRSessionBegin(NULL, session_begin_params, &errcode); //听写不需要语法，第一个参数为NULL
	if (0 != errcode)
	{
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[deepasr] BeginSession failed! error code:%d\n", errcode);
		return FALSE;
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] BeginSession suceess!");
	
	recog_channel->last_result = NULL;
	recog_channel->recog_started = FALSE;

	recog_channel->recog_request = request;
	return TRUE;
}

void deepasr_recog_end_session(deepasr_recog_channel_t *recog_channel){
	if(recog_channel->session_id) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] EndSession suceess!");
    int errorCode;
		bool is_succ = end_session(recog_channel->session_id, "mrcp channel closed", &errorCode);
    if (0 != errorCode)
    {
      apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[deepasr] BeginSession failed! error code:%d\n", errorCode);
    }
		recog_channel->session_id = NULL;
	}
}


/** Process STOP request */
static apt_bool_t deepasr_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process STOP request */
	deepasr_recog_channel_t *recog_channel = channel->method_obj;
	deepasr_recog_end_session(recog_channel);
	/* store STOP request, make sure there is no more activity and only then send the response */
	recog_channel->stop_response = response;
	return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t deepasr_recog_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	deepasr_recog_channel_t *recog_channel = channel->method_obj;
	recog_channel->timers_started = TRUE;
	return mrcp_engine_channel_message_send(channel,response);
}

/** Dispatch MRCP request */
static apt_bool_t deepasr_recog_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case RECOGNIZER_SET_PARAMS:
			break;
		case RECOGNIZER_GET_PARAMS:
			break;
		case RECOGNIZER_DEFINE_GRAMMAR:
			break;
		case RECOGNIZER_RECOGNIZE:
			processed = deepasr_recog_channel_recognize(channel,request,response);
			break;
		case RECOGNIZER_GET_RESULT:
			break;
		case RECOGNIZER_START_INPUT_TIMERS:
			processed = deepasr_recog_channel_timers_start(channel,request,response);
			break;
		case RECOGNIZER_STOP:
			processed = deepasr_recog_channel_stop(channel,request,response);
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t deepasr_recog_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t deepasr_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	deepasr_recog_channel_t *recog_channel = stream->obj;
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t deepasr_recog_stream_close(mpf_audio_stream_t *stream)
{
	deepasr_recog_channel_t *recog_channel = stream->obj;
	return TRUE;
}

/* Raise deepasr START-OF-INPUT event */
static apt_bool_t deepasr_recog_start_of_input(deepasr_recog_channel_t *recog_channel)
{
	/* create START-OF-INPUT event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_START_OF_INPUT,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/* Load deepasr recognition result */
static apt_bool_t deepasr_recog_result_load(deepasr_recog_channel_t *recog_channel, mrcp_message_t *message)
{
	apt_str_t *body = &message->body;
	if(!recog_channel->last_result) {
		return FALSE;
	}

	body->buf = apr_psprintf(message->pool,
		"<?xml version=\"1.0\"?>\n"
		"<result>\n"
		"  <interpretation confidence=\"%d\">\n"
		"    <instance>%s</instance>\n"
		"    <input mode=\"speech\">%s</input>\n"
		"  </interpretation>\n"
		"</result>\n",
		99,
		recog_channel->last_result,
		recog_channel->last_result);
	if(body->buf) {
		mrcp_generic_header_t *generic_header;
		generic_header = mrcp_generic_header_prepare(message);
		if(generic_header) {
			/* set content type */
			apt_string_assign(&generic_header->content_type,"application/x-nlsml",message->pool);
			mrcp_generic_header_property_add(message,GENERIC_HEADER_CONTENT_TYPE);
		}
		
		body->length = strlen(body->buf);
	}
	return TRUE;
}
/* Raise deepasr RECOGNITION-COMPLETE event */
static apt_bool_t deepasr_recog_recognition_complete(deepasr_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause)
{
	deepasr_recog_stream_recog(recog_channel, NULL, 0);
	deepasr_recog_end_session(recog_channel);
	mrcp_recog_header_t *recog_header;
	/* create RECOGNITION-COMPLETE event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_RECOGNITION_COMPLETE,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* get/allocate recognizer header */
	recog_header = mrcp_resource_header_prepare(message);
	if(recog_header) {
		/* set completion cause */
		recog_header->completion_cause = cause;
		mrcp_resource_header_property_add(message,RECOGNIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	if(cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
		deepasr_recog_result_load(recog_channel,message);
	}

	recog_channel->recog_request = NULL;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

static apt_bool_t deepasr_recog_stream_recog(deepasr_recog_channel_t *recog_channel,
							   const void *voice_data,
							   unsigned int voice_len 
							   ) {
	if(FALSE == recog_channel->recog_started) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] start recog");
		recog_channel->recog_started = TRUE;
	}

	if(NULL == recog_channel->session_id) {
		return FALSE;
	}

  bool is_last= false;

  if (voice_len == 0){
    is_last = true;
  }
  int errorCode;

  bool ret = false;

  if (! is_last) {
    apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] we got voice_data[%d] with Binary", voice_len);
    ret = write_data(recog_channel->session_id, voice_data, voice_len, true, &errorCode);
  } else {
    apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] we got voice_data[%d] with Text[%d]", 0, 3);
    ret = write_data(recog_channel->session_id, "EOS", 3, false, &errorCode);
  }
	if (0 == ret)
	{
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[deepasr] WriteData failed! error code:%d", ret);
		return FALSE;
	}
	//if(!= rec_stat && MSP_AUDIO_SAMPLE_LAST != aud_stat) {
	//	// apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] no need recog,rec_stat=%d,aud_stat=%d",rec_stat,aud_stat);
	//	return TRUE;
	//}
  if (! is_last) {
    apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] We're not hitting the last, will not query the result");
    return TRUE;
  }

  apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] Hitting Last, querying the final result");
  int timeout = 50;
	while (timeout > 0) 
	{
    timeout --;

		const char *rslt = get_recog_result(recog_channel->session_id);
		//const char *rslt = get_recog_result(recog_channel->session_id, &rec_stat, 0, &ret);
		if (0 == rslt)
		{
			//apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] QISRGetResult Processing, error code: %d", ret);
      usleep(100*1000);
		}else{
			if(NULL == recog_channel->last_result) {
				recog_channel->last_result = apr_pstrdup(recog_channel->channel->pool,rslt);
			} else {
				// recog_channel->last_result = apr_psprintf(recog_channel->channel->pool,"%s%s",recog_channel->last_result,rslt);
				recog_channel->last_result = apr_pstrcat(recog_channel->channel->pool, recog_channel->last_result,rslt);
			}
      break;
		}
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] Get recog result:%s",rslt);
	}
  if (NULL == recog_channel->last_result){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[deepasr] Get No recog result after 5 seconds, returning with empty");
    recog_channel->last_result = apr_pstrdup(recog_channel->channel->pool, "");
  }
	return TRUE;
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t deepasr_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{

  apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] Into STREAM WRITE with frame[type:%d, maker:%d]", frame->type, frame->marker);
	deepasr_recog_channel_t *recog_channel = stream->obj;
	if(recog_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(recog_channel->channel,recog_channel->stop_response);
		recog_channel->stop_response = NULL;
		recog_channel->recog_request = NULL;
		return TRUE;
	}
	if(frame->codec_frame.size) {
		deepasr_recog_stream_recog(recog_channel, frame->codec_frame.buffer, frame->codec_frame.size);
	}
	if(recog_channel->recog_request) {
    apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] Into MPF DETECCT");
		mpf_detector_event_e det_event = mpf_activity_detector_process(recog_channel->detector,frame);
		switch(det_event) {
			case MPF_DETECTOR_EVENT_ACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Activity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				deepasr_recog_start_of_input(recog_channel);
				break;
			case MPF_DETECTOR_EVENT_INACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Inactivity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				deepasr_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
				break;
			case MPF_DETECTOR_EVENT_NOINPUT:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Noinput " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				if(recog_channel->timers_started == TRUE) {
					deepasr_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
				}
				break;
      case MPF_DETECTOR_EVENT_NONE:
        apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[deepasr] Exit MPF DETECCT with NONE");
				break;
			default:
        break;
		}

		if(recog_channel->recog_request) {
			if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
				if(frame->marker == MPF_MARKER_START_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Start of Event " APT_SIDRES_FMT " id:%d",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id);
				}
				else if(frame->marker == MPF_MARKER_END_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected End of Event " APT_SIDRES_FMT " id:%d duration:%d ts",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id,
						frame->event_frame.duration);
				}
			}
		}

		if(recog_channel->audio_out) {
			fwrite(frame->codec_frame.buffer,1,frame->codec_frame.size,recog_channel->audio_out);
		}
	}
	return TRUE;
}

static apt_bool_t deepasr_recog_msg_signal(deepasr_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	deepasr_recog_channel_t *deepasr_channel = channel->method_obj;
	deepasr_recog_engine_t *deepasr_engine = deepasr_channel->deepasr_engine;
	apt_task_t *task = apt_consumer_task_base_get(deepasr_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		deepasr_recog_msg_t *deepasr_msg;
		msg->type = TASK_MSG_USER;
		deepasr_msg = (deepasr_recog_msg_t*) msg->data;

		deepasr_msg->type = type;
		deepasr_msg->channel = channel;
		deepasr_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t deepasr_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	deepasr_recog_msg_t *deepasr_msg = (deepasr_recog_msg_t*)msg->data;
	switch(deepasr_msg->type) {
		case DEEPASR_RECOG_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(deepasr_msg->channel,TRUE);
			break;
		case DEEPASR_RECOG_MSG_CLOSE_CHANNEL:
		{
			/* close channel, make sure there is no activity and send asynch response */
			deepasr_recog_channel_t *recog_channel = deepasr_msg->channel->method_obj;
			if(recog_channel->audio_out) {
				fclose(recog_channel->audio_out);
				recog_channel->audio_out = NULL;
			}

			mrcp_engine_channel_close_respond(deepasr_msg->channel);
			break;
		}
		case DEEPASR_RECOG_MSG_REQUEST_PROCESS:
			deepasr_recog_channel_request_dispatch(deepasr_msg->channel,deepasr_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}
