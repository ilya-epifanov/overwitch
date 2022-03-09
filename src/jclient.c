/*
 *   jclient.c
 *   Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of Overwitch.
 *
 *   Overwitch is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Overwitch is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Overwitch. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <math.h>
#include <jack/jack.h>
#include <jack/intclient.h>
#include <jack/thread.h>
#include <jack/midiport.h>

#include "jclient.h"

#define MAX_READ_FRAMES 5
#define STARTUP_TIME 5
#define LOG_TIME 2
#define RATIO_DIFF_THRES 0.00001

#define MSG_ERROR_PORT_REGISTER "Error while registering JACK port\n"

#define LATENCY_MSG_LEN 1024

#define MIDI_BUF_EVENTS 64
#define MIDI_BUF_SIZE (MIDI_BUF_EVENTS * OB_MIDI_EVENT_SIZE)

#define MAX_LATENCY (8192 * 2)	//This is twice the maximum JACK latency.

double
jclient_get_time ()
{
  return jack_get_time () * 1.0e-6;
}

size_t
jclient_buffer_read (void *buffer, char *src, size_t size)
{
  if (src)
    {
      return jack_ringbuffer_read (buffer, src, size);
    }
  else
    {
      jack_ringbuffer_read_advance (buffer, size);
      return 0;
    }
}

void
jclient_print_latencies (struct resampler *resampler, const char *end)
{
  printf
    ("%s: o2j latency: %.1f ms, max. %.1f ms; j2o latency: %.1f ms, max. %.1f ms%s",
     resampler->ow.device_desc->name,
     resampler->o2p_latency * 1000.0 / (resampler->ow.o2p_frame_size *
					OB_SAMPLE_RATE),
     resampler->o2p_max_latency * 1000.0 / (resampler->ow.o2p_frame_size *
					    OB_SAMPLE_RATE),
     resampler->p2o_latency * 1000.0 / (resampler->ow.p2o_frame_size *
					OB_SAMPLE_RATE),
     resampler->p2o_max_latency * 1000.0 / (resampler->ow.p2o_frame_size *
					    OB_SAMPLE_RATE), end);
}

void
resampler_reset_buffers (struct resampler *resampler)
{
  size_t p2o_bufsize = resampler->bufsize * resampler->ow.p2o_frame_size;
  size_t o2p_bufsize = resampler->bufsize * resampler->ow.o2p_frame_size;
  if (resampler->p2o_buf_in)
    {
      free (resampler->p2o_buf_in);
      free (resampler->p2o_buf_out);
      free (resampler->p2o_aux);
      free (resampler->p2o_queue);
      free (resampler->o2p_buf_in);
      free (resampler->o2p_buf_out);
    }

  //The 8 times scale allow up to more than 192 kHz sample rate in JACK.
  resampler->p2o_buf_in = malloc (p2o_bufsize * 8);
  resampler->p2o_buf_out = malloc (p2o_bufsize * 8);
  resampler->p2o_aux = malloc (p2o_bufsize);
  resampler->p2o_queue = malloc (p2o_bufsize * 8);
  resampler->p2o_queue_len = 0;

  resampler->o2p_buf_in = malloc (o2p_bufsize);
  resampler->o2p_buf_out = malloc (o2p_bufsize);

  memset (resampler->p2o_buf_in, 0, p2o_bufsize);
  memset (resampler->o2p_buf_in, 0, o2p_bufsize);

  resampler->o2p_buf_size = resampler->bufsize * resampler->ow.o2p_frame_size;
  resampler->p2o_buf_size = resampler->bufsize * resampler->ow.p2o_frame_size;

  resampler->p2o_max_latency = 0;
  resampler->o2p_max_latency = 0;
  resampler->p2o_latency = 0;
  resampler->o2p_latency = 0;
  resampler->reading_at_o2p_end = 0;
}

void
jclient_reset_buffers (struct jclient *jclient)
{
  size_t rso2j, bytes;
  resampler_reset_buffers (&jclient->resampler);
  rso2j = jack_ringbuffer_read_space (jclient->o2p_audio_rb);
  bytes =
    overwitch_bytes_to_frame_bytes (rso2j,
				    jclient->resampler.ow.o2p_frame_size);
  jack_ringbuffer_read_advance (jclient->o2p_audio_rb, bytes);
}

void
resampler_reset_dll (struct resampler *resampler,
		     jack_nframes_t new_samplerate)
{
  static int init = 0;
  if (!init || overwitch_get_status (&resampler->ow) < OW_STATUS_RUN)
    {
      debug_print (2, "Initializing dll...\n");
      dll_init (&resampler->dll, new_samplerate, OB_SAMPLE_RATE,
		resampler->bufsize, resampler->ow.frames_per_transfer);
      overwitch_set_status (&resampler->ow, OW_STATUS_READY);
      init = 1;
    }
  else
    {
      debug_print (2, "Just adjusting dll ratio...\n");
      resampler->dll.ratio =
	resampler->dll.last_ratio_avg * new_samplerate /
	resampler->samplerate;
      overwitch_set_status (&resampler->ow, OW_STATUS_READY);
      resampler->log_cycles = 0;
      resampler->log_control_cycles =
	STARTUP_TIME * new_samplerate / resampler->bufsize;
    }
  resampler->o2p_ratio = resampler->dll.ratio;
  resampler->samplerate = new_samplerate;
}

static int
jclient_thread_xrun_cb (void *cb_data)
{
  struct jclient *jclient = cb_data;
  error_print ("JACK xrun\n");
  pthread_spin_lock (&jclient->resampler.lock);
  jclient->resampler.xruns++;
  pthread_spin_unlock (&jclient->resampler.lock);
  return 0;
}

static void
jclient_port_connect_cb (jack_port_id_t a, jack_port_id_t b, int connect,
			 void *cb_data)
{
  struct jclient *jclient = cb_data;
  int p2o_enabled = 0;
  //We only check for j2o (imput) ports as o2j must always be running.
  for (int i = 0; i < jclient->resampler.ow.device_desc->inputs; i++)
    {
      if (jack_port_connected (jclient->input_ports[i]))
	{
	  p2o_enabled = 1;
	  break;
	}
    }
  overwitch_set_p2o_audio_enable (&jclient->resampler.ow, p2o_enabled);
}

static void
jclient_jack_shutdown_cb (jack_status_t code, const char *reason,
			  void *cb_data)
{
  struct jclient *jclient = cb_data;
  jclient_exit (jclient);
}

static int
jclient_set_buffer_size_cb (jack_nframes_t nframes, void *cb_data)
{
  struct jclient *jclient = cb_data;
  if (jclient->resampler.bufsize != nframes)
    {
      printf ("JACK buffer size: %d\n", nframes);
      jclient->resampler.bufsize = nframes;
      jclient_reset_buffers (jclient);
      resampler_reset_dll (&jclient->resampler,
			   jclient->resampler.samplerate);
    }
  return 0;
}

static int
jclient_set_sample_rate_cb (jack_nframes_t nframes, void *cb_data)
{
  struct jclient *jclient = cb_data;
  if (jclient->resampler.samplerate != nframes)
    {
      printf ("JACK sample rate: %d\n", nframes);
      if (jclient->resampler.p2o_buf_in)	//This means that jclient_reset_buffers has been called and thus bufsize has been set.
	{
	  resampler_reset_dll (&jclient->resampler, nframes);
	}
      else
	{
	  jclient->resampler.samplerate = nframes;
	}
    }
  return 0;
}

static long
jclient_p2o_reader (void *cb_data, float **data)
{
  long ret;
  struct resampler *resampler = cb_data;

  *data = resampler->p2o_buf_in;

  if (resampler->p2o_queue_len == 0)
    {
      debug_print (2, "j2o: Can not read data from queue\n");
      return resampler->bufsize;
    }

  ret = resampler->p2o_queue_len;
  memcpy (resampler->p2o_buf_in, resampler->p2o_queue,
	  ret * resampler->ow.p2o_frame_size);
  resampler->p2o_queue_len = 0;

  return ret;
}

static long
resampler_o2p_reader (void *cb_data, float **data)
{
  size_t rso2j;
  size_t bytes;
  long frames;
  static int last_frames = 1;
  struct resampler *resampler = cb_data;

  *data = resampler->o2p_buf_in;

  rso2j = resampler->ow.buffer_read_space (resampler->ow.o2p_audio_buf);
  if (resampler->reading_at_o2p_end)
    {
      resampler->o2p_latency = rso2j;
      if (resampler->o2p_latency > resampler->o2p_max_latency)
	{
	  resampler->o2p_max_latency = resampler->o2p_latency;
	}

      if (rso2j >= resampler->ow.o2p_frame_size)
	{
	  frames = rso2j / resampler->ow.o2p_frame_size;
	  frames = frames > MAX_READ_FRAMES ? MAX_READ_FRAMES : frames;
	  bytes = frames * resampler->ow.o2p_frame_size;
	  resampler->ow.buffer_read (resampler->ow.o2p_audio_buf,
				     (void *) resampler->o2p_buf_in, bytes);
	}
      else
	{
	  debug_print (2,
		       "o2j: Audio ring buffer underflow (%zu < %zu). Replicating last sample...\n",
		       rso2j, resampler->ow.o2p_transfer_size);
	  if (last_frames > 1)
	    {
	      uint64_t pos =
		(last_frames - 1) * resampler->ow.device_desc->outputs;
	      memcpy (resampler->o2p_buf_in, &resampler->o2p_buf_in[pos],
		      resampler->ow.o2p_frame_size);
	    }
	  frames = MAX_READ_FRAMES;
	}
    }
  else
    {
      if (rso2j >= resampler->o2p_buf_size)
	{
	  debug_print (2, "o2j: Emptying buffer and running...\n");
	  bytes =
	    overwitch_bytes_to_frame_bytes (rso2j, resampler->o2p_buf_size);
	  resampler->ow.buffer_read (resampler->ow.o2p_audio_buf, NULL,
				     bytes);
	  resampler->reading_at_o2p_end = 1;
	}
      frames = MAX_READ_FRAMES;
    }

  resampler->dll.kj += frames;
  last_frames = frames;
  return frames;
}

static inline void
resampler_o2j (struct resampler *resampler)
{
  long gen_frames;

  gen_frames = src_callback_read (resampler->o2p_state, resampler->o2p_ratio,
				  resampler->bufsize, resampler->o2p_buf_out);
  if (gen_frames != resampler->bufsize)
    {
      error_print
	("o2j: Unexpected frames with ratio %f (output %ld, expected %d)\n",
	 resampler->o2p_ratio, gen_frames, resampler->bufsize);
    }
}

static inline void
resampler_j2o (struct resampler *resampler)
{
  long gen_frames;
  int inc;
  int frames;
  size_t bytes;
  size_t wsj2o;
  static double p2o_acc = .0;

  memcpy (&resampler->p2o_queue
	  [resampler->p2o_queue_len *
	   resampler->ow.p2o_frame_size], resampler->p2o_aux,
	  resampler->p2o_buf_size);
  resampler->p2o_queue_len += resampler->bufsize;

  p2o_acc += resampler->bufsize * (resampler->p2o_ratio - 1.0);
  inc = trunc (p2o_acc);
  p2o_acc -= inc;
  frames = resampler->bufsize + inc;

  gen_frames =
    src_callback_read (resampler->p2o_state,
		       resampler->p2o_ratio, frames, resampler->p2o_buf_out);
  if (gen_frames != frames)
    {
      error_print
	("j2o: Unexpected frames with ratio %f (output %ld, expected %d)\n",
	 resampler->p2o_ratio, gen_frames, frames);
    }

  if (resampler->status < RES_STATUS_RUN)
    {
      return;
    }

  bytes = gen_frames * resampler->ow.p2o_frame_size;
  wsj2o = resampler->ow.buffer_write_space (resampler->ow.p2o_audio_buf);

  if (bytes <= wsj2o)
    {
      resampler->ow.buffer_write (resampler->ow.p2o_audio_buf,
				  (void *) resampler->p2o_buf_out, bytes);
    }
  else
    {
      error_print ("j2o: Audio ring buffer overflow. Discarding data...\n");
    }
}

static inline int
resampler_compute_ratios (struct resampler *resampler, double time)
{
  int xruns;
  overwitch_status_t ow_status;
  static char latency_msg[LATENCY_MSG_LEN];
  struct dll *dll = &resampler->dll;

  pthread_spin_lock (&resampler->lock);
  xruns = resampler->xruns;
  resampler->xruns = 0;
  pthread_spin_unlock (&resampler->lock);

  pthread_spin_lock (&resampler->ow.lock);
  resampler->p2o_latency = resampler->ow.p2o_latency;
  resampler->p2o_max_latency = resampler->ow.p2o_max_latency;
  dll_load_dll_overwitch (dll);
  pthread_spin_unlock (&resampler->ow.lock);

  ow_status = overwitch_get_status (&resampler->ow);
  if (resampler->status == RES_STATUS_READY && ow_status <= OW_STATUS_BOOT)
    {
      if (ow_status == OW_STATUS_READY)
	{
	  overwitch_set_status (&resampler->ow, OW_STATUS_BOOT);
	  debug_print (2, "Booting Overbridge side...\n");
	}
      return 1;
    }

  if (resampler->status == RES_STATUS_READY && ow_status == OW_STATUS_WAIT)
    {
      dll_update_err (dll, time);
      dll_first_time_run (dll);

      debug_print (2, "Starting up...\n");
      dll_set_loop_filter (dll, 1.0, resampler->bufsize,
			   resampler->samplerate);
      resampler->status = RES_STATUS_BOOT;

      resampler->log_cycles = 0;
      resampler->log_control_cycles =
	STARTUP_TIME * resampler->samplerate / resampler->bufsize;
      return 0;
    }

  if (xruns)
    {
      debug_print (2, "Fixing %d xruns...\n", xruns);

      //With this, we try to recover from the unreaded frames that are in the o2j buffer and...
      resampler->o2p_ratio = dll->ratio * (1 + xruns);
      resampler->p2o_ratio = 1.0 / resampler->o2p_ratio;
      resampler_o2j (resampler);

      resampler->p2o_max_latency = 0;
      resampler->o2p_max_latency = 0;

      //... we skip the current cycle dll update as time masurements are not precise enough and would lead to errors.
      return 0;
    }

  dll_update_err (dll, time);
  dll_update (dll);

  if (dll->ratio < 0.0)
    {
      error_print ("Negative ratio detected. Stopping...\n");
      overwitch_set_status (&resampler->ow, OW_STATUS_ERROR);
      return 1;
    }

  resampler->o2p_ratio = dll->ratio;
  resampler->p2o_ratio = 1.0 / resampler->o2p_ratio;

  resampler->log_cycles++;
  if (resampler->log_cycles == resampler->log_control_cycles)
    {
      dll_calc_avg (dll, resampler->log_control_cycles);

      if (debug_level)
	{
	  snprintf (latency_msg, LATENCY_MSG_LEN,
		    "; o2j ratio: %f, avg. %f\n", dll->ratio, dll->ratio_avg);
	  jclient_print_latencies (resampler, latency_msg);
	}

      resampler->log_cycles = 0;

      if (resampler->status == RES_STATUS_BOOT)
	{
	  debug_print (2, "Tunning...\n");
	  dll_set_loop_filter (dll, 0.05, resampler->bufsize,
			       resampler->samplerate);
	  resampler->status = RES_STATUS_TUNE;
	  resampler->log_control_cycles =
	    LOG_TIME * resampler->samplerate / resampler->bufsize;
	}

      if (resampler->status == RES_STATUS_TUNE
	  && fabs (dll->ratio_avg - dll->last_ratio_avg) < RATIO_DIFF_THRES)
	{
	  debug_print (2, "Running...\n");
	  dll_set_loop_filter (dll, 0.02, resampler->bufsize,
			       resampler->samplerate);
	  resampler->status = RES_STATUS_RUN;
	  overwitch_set_status (&resampler->ow, OW_STATUS_RUN);
	}
    }

  return 0;
}

static inline void
jclient_o2p_midi (struct jclient *jclient, jack_nframes_t nframes)
{
  size_t data_size;
  void *midi_port_buf;
  jack_midi_data_t *jmidi;
  struct overwitch_midi_event event;
  jack_nframes_t last_frames, frames, last_frame_time, event_frames;

  midi_port_buf = jack_port_get_buffer (jclient->midi_output_port, nframes);
  jack_midi_clear_buffer (midi_port_buf);
  last_frames = 0;

  while (jack_ringbuffer_read_space (jclient->o2p_midi_rb) >=
	 sizeof (struct overwitch_midi_event))
    {
      jack_ringbuffer_peek (jclient->o2p_midi_rb, (void *) &event,
			    sizeof (struct overwitch_midi_event));

      event_frames = jack_time_to_frames (jclient->client, event.time);
      last_frame_time = jack_time_to_frames (jclient->client, event.time);

      if (last_frame_time < event_frames)
	{
	  debug_print (2, "Event delayed: %u frames\n",
		       event_frames - last_frame_time);
	  frames = 0;
	}
      else
	{
	  frames =
	    (last_frame_time - event_frames) % jclient->resampler.bufsize;
	}

      debug_print (2, "Event frames: %u\n", frames);

      if (frames < last_frames)
	{
	  debug_print (2, "Skipping until the next cycle...\n");
	  last_frames = 0;
	  break;
	}
      last_frames = frames;

      jack_ringbuffer_read_advance (jclient->o2p_midi_rb,
				    sizeof (struct overwitch_midi_event));

      if (event.bytes[0] == 0x0f)
	{
	  data_size = 1;
	}
      else
	{
	  data_size = 3;
	}
      jmidi = jack_midi_event_reserve (midi_port_buf, frames, data_size);
      if (jmidi)
	{
	  jmidi[0] = event.bytes[1];
	  if (data_size == 3)
	    {
	      jmidi[1] = event.bytes[2];
	      jmidi[2] = event.bytes[3];
	    }
	}
    }
}

static inline void
jclient_p2o_midi (struct jclient *jclient, jack_nframes_t nframes)
{
  jack_midi_event_t jevent;
  void *midi_port_buf;
  struct overwitch_midi_event oevent;
  jack_nframes_t event_count;
  jack_midi_data_t status_byte;

  if (jclient->resampler.status < RES_STATUS_RUN)
    {
      return;
    }

  midi_port_buf = jack_port_get_buffer (jclient->midi_input_port, nframes);
  event_count = jack_midi_get_event_count (midi_port_buf);

  for (int i = 0; i < event_count; i++)
    {
      oevent.bytes[0] = 0;
      jack_midi_event_get (&jevent, midi_port_buf, i);
      status_byte = jevent.buffer[0];

      if (jevent.size == 1 && status_byte >= 0xf8 && status_byte <= 0xfc)
	{
	  oevent.bytes[0] = 0x0f;	//Single Byte
	  oevent.bytes[1] = jevent.buffer[0];
	}
      else if (jevent.size == 2)
	{
	  switch (status_byte & 0xf0)
	    {
	    case 0xc0:		//Program Change
	      oevent.bytes[0] = 0x0c;
	      break;
	    case 0xd0:		//Channel Pressure (After-touch)
	      oevent.bytes[0] = 0x0d;
	      break;
	    }
	  oevent.bytes[1] = jevent.buffer[0];
	  oevent.bytes[2] = jevent.buffer[1];
	}
      else if (jevent.size == 3)
	{
	  switch (status_byte & 0xf0)
	    {
	    case 0x80:		//Note Off
	      oevent.bytes[0] = 0x08;
	      break;
	    case 0x90:		//Note On
	      oevent.bytes[0] = 0x09;
	      break;
	    case 0xa0:		//Polyphonic Key Pressure
	      oevent.bytes[0] = 0x0a;
	      break;
	    case 0xb0:		//Control Change
	      oevent.bytes[0] = 0x0b;
	      break;
	    case 0xe0:		//Pitch Bend Change
	      oevent.bytes[0] = 0x0e;
	      break;
	    }
	  oevent.bytes[1] = jevent.buffer[0];
	  oevent.bytes[2] = jevent.buffer[1];
	  oevent.bytes[3] = jevent.buffer[2];
	}

      oevent.time = jack_frames_to_time (jclient->client, jevent.time);

      if (oevent.bytes[0])
	{
	  if (jack_ringbuffer_write_space (jclient->p2o_midi_rb) >=
	      sizeof (struct overwitch_midi_event))
	    {
	      jack_ringbuffer_write (jclient->p2o_midi_rb,
				     (void *) &oevent,
				     sizeof (struct overwitch_midi_event));
	    }
	  else
	    {
	      error_print
		("j2o: MIDI ring buffer overflow. Discarding data...\n");
	    }
	}
    }
}

static inline int
jclient_process_cb (jack_nframes_t nframes, void *arg)
{
  float *f;
  jack_default_audio_sample_t *buffer[OB_MAX_TRACKS];
  struct jclient *jclient = arg;

  jack_nframes_t current_frames;
  jack_time_t current_usecs;
  jack_time_t next_usecs;
  float period_usecs;
  double time;

  if (jack_get_cycle_times (jclient->client,
			    &current_frames,
			    &current_usecs, &next_usecs, &period_usecs))
    {
      error_print ("Error while getting JACK time\n");
    }

  time = current_usecs * 1.0e-6;

  if (resampler_compute_ratios (&jclient->resampler, time))
    {
      return 0;
    }

  resampler_o2j (&jclient->resampler);

  //o2j

  f = jclient->resampler.o2p_buf_out;
  for (int i = 0; i < jclient->resampler.ow.device_desc->outputs; i++)
    {
      buffer[i] = jack_port_get_buffer (jclient->output_ports[i], nframes);
    }
  for (int i = 0; i < nframes; i++)
    {
      for (int j = 0; j < jclient->resampler.ow.device_desc->outputs; j++)
	{
	  buffer[j][i] = *f;
	  f++;
	}
    }

  //j2o

  if (overwitch_is_p2o_audio_enable (&jclient->resampler.ow))
    {
      f = jclient->resampler.p2o_aux;
      for (int i = 0; i < jclient->resampler.ow.device_desc->inputs; i++)
	{
	  buffer[i] = jack_port_get_buffer (jclient->input_ports[i], nframes);
	}
      for (int i = 0; i < nframes; i++)
	{
	  for (int j = 0; j < jclient->resampler.ow.device_desc->inputs; j++)
	    {
	      *f = buffer[j][i];
	      f++;
	    }
	}

      resampler_j2o (&jclient->resampler);
    }

  jclient_o2p_midi (jclient, nframes);

  jclient_p2o_midi (jclient, nframes);

  return 0;
}

static void
set_rt_priority (pthread_t * thread, int priority)
{
  int err = jack_acquire_real_time_scheduling (*thread, priority);
  if (err)
    {
      error_print ("Could not set real time priority\n");
    }
}

void
jclient_exit (struct jclient *jclient)
{
  jclient_print_latencies (&jclient->resampler, "\n");
  overwitch_set_status (&jclient->resampler.ow, OW_STATUS_STOP);
}

int
jclient_run (struct jclient *jclient)
{
  jack_options_t options = JackNoStartServer;
  jack_status_t status;
  overwitch_err_t ob_status;
  char *client_name;

  jclient->resampler.samplerate = 0;
  jclient->resampler.bufsize = 0;
  jclient->resampler.xruns = 0;
  jclient->resampler.p2o_buf_in = NULL;

  jclient->resampler.status = RES_STATUS_READY;

  //The so-called Overwitch API
  jclient->resampler.ow.buffer_read_space =
    (overwitch_buffer_rw_space_t) jack_ringbuffer_read_space;
  jclient->resampler.ow.buffer_write_space =
    (overwitch_buffer_rw_space_t) jack_ringbuffer_write_space;
  jclient->resampler.ow.buffer_read = jclient_buffer_read;
  jclient->resampler.ow.buffer_write =
    (overwitch_buffer_write_t) jack_ringbuffer_write;
  jclient->resampler.ow.get_time = jclient_get_time;
  jclient->resampler.ow.dll_ow = &jclient->resampler.dll.dll_ow;

  ob_status =
    overwitch_init (&jclient->resampler.ow, jclient->bus, jclient->address,
		    jclient->blocks_per_transfer,
		    OW_OPTION_MIDI | OW_OPTION_SECONDARY_DLL);
  if (ob_status)
    {
      error_print ("Overwitch error: %s\n",
		   overbrigde_get_err_str (ob_status));
      goto end;
    }

  jclient->client =
    jack_client_open (jclient->resampler.ow.device_desc->name, options,
		      &status, NULL);
  if (jclient->client == NULL)
    {
      error_print ("jack_client_open() failed, status = 0x%2.0x\n", status);

      if (status & JackServerFailed)
	{
	  error_print ("Unable to connect to JACK server\n");
	}

      goto cleanup_overwitch;
    }

  if (status & JackServerStarted)
    {
      debug_print (1, "JACK server started\n");
    }

  if (status & JackNameNotUnique)
    {
      client_name = jack_get_client_name (jclient->client);
      debug_print (0, "Name client in use. Using %s...\n", client_name);
    }

  if (jack_set_process_callback
      (jclient->client, jclient_process_cb, jclient))
    {
      goto cleanup_jack;
    }

  pthread_spin_init (&jclient->resampler.lock, PTHREAD_PROCESS_SHARED);
  if (jack_set_xrun_callback
      (jclient->client, jclient_thread_xrun_cb, jclient))
    {
      goto cleanup_jack;
    }

  if (jack_set_port_connect_callback (jclient->client,
				      jclient_port_connect_cb, jclient))
    {
      error_print
	("Cannot set port connect callback so j2o audio will not be possible\n");
    }

  jack_on_info_shutdown (jclient->client, jclient_jack_shutdown_cb, jclient);

  if (jack_set_sample_rate_callback
      (jclient->client, jclient_set_sample_rate_cb, jclient))
    {
      goto cleanup_jack;
    }

  if (jack_set_buffer_size_callback
      (jclient->client, jclient_set_buffer_size_cb, jclient))
    {
      goto cleanup_jack;
    }

  if (jclient->priority < 0)
    {
      jclient->priority = jack_client_real_time_priority (jclient->client);
    }
  debug_print (1, "Using RT priority %d...\n", jclient->priority);

  jclient->output_ports =
    malloc (sizeof (jack_port_t *) *
	    jclient->resampler.ow.device_desc->outputs);
  for (int i = 0; i < jclient->resampler.ow.device_desc->outputs; i++)
    {
      jclient->output_ports[i] =
	jack_port_register (jclient->client,
			    jclient->resampler.ow.
			    device_desc->output_track_names[i],
			    JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

      if (jclient->output_ports[i] == NULL)
	{
	  error_print (MSG_ERROR_PORT_REGISTER);
	  goto cleanup_jack;
	}
    }

  jclient->input_ports =
    malloc (sizeof (jack_port_t *) *
	    jclient->resampler.ow.device_desc->inputs);
  for (int i = 0; i < jclient->resampler.ow.device_desc->inputs; i++)
    {
      jclient->input_ports[i] =
	jack_port_register (jclient->client,
			    jclient->resampler.ow.
			    device_desc->input_track_names[i],
			    JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

      if (jclient->input_ports[i] == NULL)
	{
	  error_print (MSG_ERROR_PORT_REGISTER);
	  goto cleanup_jack;
	}
    }

  jclient->midi_output_port =
    jack_port_register (jclient->client, "MIDI out", JACK_DEFAULT_MIDI_TYPE,
			JackPortIsOutput, 0);

  if (jclient->midi_output_port == NULL)
    {
      error_print (MSG_ERROR_PORT_REGISTER);
      goto cleanup_jack;
    }

  jclient->midi_input_port =
    jack_port_register (jclient->client, "MIDI in", JACK_DEFAULT_MIDI_TYPE,
			JackPortIsInput, 0);

  if (jclient->midi_input_port == NULL)
    {
      error_print (MSG_ERROR_PORT_REGISTER);
      goto cleanup_jack;
    }

  //Resamplers

  jclient->resampler.p2o_state =
    src_callback_new (jclient_p2o_reader, jclient->quality,
		      jclient->resampler.ow.device_desc->inputs, NULL,
		      &jclient->resampler);
  jclient->resampler.o2p_state =
    src_callback_new (resampler_o2p_reader, jclient->quality,
		      jclient->resampler.ow.device_desc->outputs, NULL,
		      &jclient->resampler);

  //Ring buffers

  jclient->o2p_audio_rb =
    jack_ringbuffer_create (MAX_LATENCY *
			    jclient->resampler.ow.o2p_frame_size);
  jack_ringbuffer_mlock (jclient->o2p_audio_rb);
  jclient->resampler.ow.o2p_audio_buf = jclient->o2p_audio_rb;

  jclient->p2o_audio_rb =
    jack_ringbuffer_create (MAX_LATENCY *
			    jclient->resampler.ow.p2o_frame_size);
  jack_ringbuffer_mlock (jclient->p2o_audio_rb);
  jclient->resampler.ow.p2o_audio_buf = jclient->p2o_audio_rb;

  jclient->p2o_midi_rb = jack_ringbuffer_create (MIDI_BUF_SIZE * 8);
  jack_ringbuffer_mlock (jclient->p2o_midi_rb);
  jclient->resampler.ow.p2o_midi_buf = jclient->p2o_midi_rb;

  jclient->o2p_midi_rb = jack_ringbuffer_create (MIDI_BUF_SIZE * 8);
  jack_ringbuffer_mlock (jclient->o2p_midi_rb);
  jclient->resampler.ow.o2p_midi_buf = jclient->o2p_midi_rb;

  if (overwitch_activate (&jclient->resampler.ow))
    {
      goto cleanup_jack;
    }

  set_rt_priority (&jclient->resampler.ow.p2o_midi_thread, jclient->priority);
  set_rt_priority (&jclient->resampler.ow.audio_o2p_midi_thread,
		   jclient->priority);

  jclient_set_sample_rate_cb (jack_get_sample_rate (jclient->client),
			      jclient);
  jclient_set_buffer_size_cb (jack_get_buffer_size (jclient->client),
			      jclient);

  if (jack_activate (jclient->client))
    {
      error_print ("Cannot activate client\n");
      goto cleanup_jack;
    }

  overwitch_wait (&jclient->resampler.ow);

  debug_print (1, "Exiting...\n");
  jack_deactivate (jclient->client);

cleanup_jack:
  jack_ringbuffer_free (jclient->p2o_audio_rb);
  jack_ringbuffer_free (jclient->o2p_audio_rb);
  jack_ringbuffer_free (jclient->p2o_midi_rb);
  jack_ringbuffer_free (jclient->o2p_midi_rb);
  jack_client_close (jclient->client);
  src_delete (jclient->resampler.p2o_state);
  src_delete (jclient->resampler.o2p_state);
  free (jclient->output_ports);
  free (jclient->input_ports);
  free (jclient->resampler.p2o_buf_in);
  free (jclient->resampler.p2o_buf_out);
  free (jclient->resampler.p2o_aux);
  free (jclient->resampler.p2o_queue);
  free (jclient->resampler.o2p_buf_in);
  free (jclient->resampler.o2p_buf_out);
  pthread_spin_destroy (&jclient->resampler.lock);
cleanup_overwitch:
  overwitch_destroy (&jclient->resampler.ow);

end:
  return jclient->resampler.status;
}

void *
jclient_run_thread (void *data)
{
  struct jclient *jclient = data;
  jclient_run (jclient);
  return NULL;
}
