/******************************************************
 *
 * freenect - implementation file
 *
 * (c) copyright 2011/2012 Matthias Kronlachner
 *
 *
 *   institute of electronic music and acoustics (iem)
 *
 ******************************************************
 *
 * license: GNU General Public License v.2
 *
 ******************************************************/

#include "m_pd.h"
#include <string.h>
#include <pthread.h>

#include "libfreenect.h"
#include "libfreenect-audio.h"
//#include <stdio.h>
//#include <signal.h>

/* general */

static freenect_context* f_ctx;
static freenect_device* f_dev;

#define FREENECT_BUFFER 5000 //milliseconds BUFFER
#define FREENECT_SAMPLERATE 16000.0 //SAMPLERATE

static const double const_1_div_2147483648_ = 1.0 / 2147483648.0; /* 32 bit multiplier */

typedef struct _freenect
{
  t_object x_obj;

  float*x_buffer1;        /* audio buffers */
  float*x_buffer2;
  float*x_buffer3;
  float*x_buffer4;
  
  unsigned int x_bufsize; /* length of the buffer */

  unsigned int x_freenect_pos; /* buffer pos for callback */
  
  unsigned int x_playback_pos; /* buffer pos for playback */
  
  unsigned int x_num_samples; /* how many "active" samples in buffer */
  
  float rate_conv; // 16kHz/Samplingrate
  
  int x_ready;           /* flag: if true, start playback */

	int exit_thread;
	
	time_t last_timestamp;
	double EstimatedSRate;
	double AverageSRate;
	
  pthread_mutex_t   x_mutex; /* mutex to lock buffers */
  pthread_t x_freenect_thread; /* Calls freenect_process_event */

  t_outlet*x_out1;
  t_outlet*x_out2;
  t_outlet*x_out3;
  t_outlet*x_out4;
  t_outlet*x_infoout;
        
} t_freenect_audio;

/* ------------------------ freenect~ ----------------------------- */ 

static t_class *freenect_audio_class;

static void in_callback(freenect_device* dev, int num_samples,
                 int32_t* mic1, int32_t* mic2,
                 int32_t* mic3, int32_t* mic4,
                 int16_t* cancelled, void *unknown) {
					 
  t_freenect_audio*x = (t_freenect_audio*)freenect_get_user(dev); // Get Struct (x->) from Libfreenect

  if (num_samples)
  {	
	pthread_mutex_lock(&x->x_mutex);
	
	
	float*buf1=x->x_buffer1;
	float*buf2=x->x_buffer2;
	float*buf3=x->x_buffer3;
	float*buf4=x->x_buffer4;
	
	unsigned int pos=x->x_freenect_pos;
	int i=0;
	
	for(i=0; i < num_samples; i++)
	{
		if(pos >= x->x_bufsize)
		{
			pos=0;
		}

		buf1[pos]=(float) ((double)mic1[i] * const_1_div_2147483648_);
		buf2[pos]=(float) ((double)mic2[i] * const_1_div_2147483648_);
		buf3[pos]=(float) ((double)mic3[i] * const_1_div_2147483648_);
		buf4[pos]=(float) ((double)mic4[i] * const_1_div_2147483648_);
		
		pos++;
	}
	
	x->x_freenect_pos=pos;
	
	if (x->x_num_samples+num_samples > x->x_bufsize)
	{
		x->x_num_samples=num_samples;
	} else {
		x->x_num_samples=x->x_num_samples+num_samples; // Received Samples - played samples
	}
	
	pthread_mutex_unlock(&x->x_mutex);


	
	//post("Sample received.  Total samples recorded: %d\n", num_samples);
	//post("Sample in buffer: %d\n", x->x_num_samples);
  }  
  if (x->x_num_samples>x->x_bufsize/2)
  {
	  x->x_ready = 1;
  } else
  {
	  x->x_ready = 0;
  }
}

static void freenect_thread_func(void*target) {
					 
  t_freenect_audio*x = (t_freenect_audio*) target;

  while ((freenect_process_events(f_ctx) >= 0) && (x->exit_thread == 0))
  {
	  // maybe update or something
  }
  //return 0;
}

/*--------------------------------------------------------------------
 * freenect_audio_bang : output signal vector
 *--------------------------------------------------------------------*/
 
static void freenect_audio_bang(t_freenect_audio*x) {
  int i=0;
  int num_samples = x->x_num_samples;

	pthread_mutex_lock(&x->x_mutex);

	float*buf1=x->x_buffer1;
	float*buf2=x->x_buffer2;
	float*buf3=x->x_buffer3;
	float*buf4=x->x_buffer4;
		
	t_atom mic1[num_samples];
	t_atom mic2[num_samples];
	t_atom mic3[num_samples];
	t_atom mic4[num_samples];
	
	int pos=x->x_freenect_pos-num_samples;
	
  while(i < num_samples) {
		if (pos<0)
		{
			pos=x->x_bufsize+pos-1;
		}
		if (pos > (int)x->x_bufsize-1)
		{
			pos=0;
		}
    SETFLOAT(mic1+i, buf1[i]);
    SETFLOAT(mic2+i, buf2[i]);
    SETFLOAT(mic3+i, buf3[i]);
    SETFLOAT(mic4+i, buf4[i]);
    i++;
    pos++;
  }
  x->x_num_samples=0; // RESET SAMPLE COUNT
  x->x_freenect_pos=0;
  
  pthread_mutex_unlock(&x->x_mutex);
  
	outlet_float(x->x_infoout, num_samples); // output number of samples
  outlet_anything(x->x_out4, gensym("list"), num_samples, mic4); // output samples in list for each channel
  outlet_anything(x->x_out3, gensym("list"), num_samples, mic3);
  outlet_anything(x->x_out2, gensym("list"), num_samples, mic2);
  outlet_anything(x->x_out1, gensym("list"), num_samples, mic1);
  
}

static void freenect_audio_free(t_freenect_audio*x){
	x->exit_thread = 1;
  freebytes(x->x_buffer1, x->x_bufsize*sizeof(float));
  freebytes(x->x_buffer2, x->x_bufsize*sizeof(float));
  freebytes(x->x_buffer3, x->x_bufsize*sizeof(float));
  freebytes(x->x_buffer4, x->x_bufsize*sizeof(float));
  
  pthread_mutex_destroy(&x->x_mutex);
  pthread_detach(&x->x_freenect_thread);
  pthread_exit(&x->x_freenect_thread);
  
  freenect_stop_audio(f_dev);
  freenect_close_device(f_dev);
  freenect_shutdown(f_ctx);
  
  if(x->x_out1)
    outlet_free(x->x_out1);
	if(x->x_out2)
    outlet_free(x->x_out2);
  if(x->x_out3)
    outlet_free(x->x_out3);
  if(x->x_out4)
    outlet_free(x->x_out4);
    

}

static void freenect_audio_info(t_freenect_audio*x) {

	post ("\n::freenect status::");
  struct freenect_device_attributes * devAttrib;
	int nr_devices = freenect_list_device_attributes(f_ctx, &devAttrib);
  post ("Number of devices found: %d", nr_devices);
	
	// display serial numbers
	const char* id;
	int i = 0;
	for(i=0; i < nr_devices; i++){
		id = devAttrib->camera_serial;
		devAttrib = devAttrib->next;
		post ("Device %d serial: %s", i, id);
	}
	freenect_free_device_attributes(devAttrib);
	
	int ret=freenect_supported_subdevices();
  
	
	if (ret & (1 << 0))
	{
		post ("libfreenect supports FREENECT_DEVICE_MOTOR (%i)", ret);
	}
	if (ret & (1 << 1))
	{
		post ("libfreenect supports FREENECT_DEVICE_CAMERA (%i)", ret);
	}
	if (ret & (1 << 2))
	{
		post ("libfreenect supports FREENECT_DEVICE_AUDIO (%i)", ret);
	}
	
		post ("\n estimated samplingrate (%d)", x->EstimatedSRate);
		post ("average samplingrate (%d)", x->AverageSRate);
}

static void *freenect_audio_new(t_symbol *s,int argc, t_atom *argv){
  t_freenect_audio *x = (t_freenect_audio *)pd_new(freenect_audio_class);

	post("freenect-audio 0.1 - 2011/12 by Matthias Kronlachner");
	
	if (freenect_init(&f_ctx, NULL) < 0) {
		post("freenect_init() failed\n");
	}
	
	freenect_set_log_level(f_ctx, FREENECT_LOG_ERROR); // LOW LOGLEVEL
	//freenect_set_log_level(f_ctx, FREENECT_LOG_SPEW); // log almost everything

  struct freenect_device_attributes * devAttrib;
	int nr_devices = freenect_list_device_attributes(f_ctx, &devAttrib);
  post ("Number of devices found: %d", nr_devices);
	
	// display serial numbers
	const char* id;
	int i = 0;
	for(i=0; i < nr_devices; i++){
		id = devAttrib->camera_serial;
		devAttrib = devAttrib->next;
		post ("Device %d serial: %s", i, id);
	}
	freenect_free_device_attributes(devAttrib);
	
	// CHECK IF LIBFREENECT WITH AUDIO SUPPORT
	int ret=freenect_supported_subdevices();
	if (ret & (1 << 2))
	{
		// AUDIO SUPPORTED -> OK!
	} else {
		post ("libfreenect doesn't supports FREENECT_DEVICE_AUDIO (%i)", ret);
		return(NULL);
	}
	
	freenect_select_subdevices(f_ctx, FREENECT_DEVICE_AUDIO);

	int openBySerial=0;
	int kinect_dev_nr = 0;
	t_symbol *serial;
	
	if (argc >= 1)
	{
		const char* test = "float";
		
		serial=atom_getsymbol(argv);
		
		if (!strncmp(serial->s_name,"float", 5))
		{
			kinect_dev_nr = (int)atom_getint(argv);
			openBySerial=0;
		} else {
			post ("test: %s", (char*)serial->s_name);
			openBySerial=1;
		}
	}
	
	if (openBySerial == 0)
	{
		verbose(1, "trying to open Kinect device nr %i...", (int)kinect_dev_nr);
		if (freenect_open_device(f_ctx, &f_dev, kinect_dev_nr) < 0) {
			post("ERROR: Could not open Kinect Nr %i !", kinect_dev_nr);
			return(NULL);
		} else
			post("Kinect Nr %d opened", kinect_dev_nr);
	}
	
	// OPEN KINECT BY SERIAL
	if (openBySerial == 1)
	{
		post("trying to open Kinect with serial %s...", (char*)serial->s_name);
		if (freenect_open_device_by_camera_serial(f_ctx, &f_dev, (char*)serial->s_name) < 0) {
			post("ERROR: Could not open Kinect with serial %s !", (char*)serial->s_name);
			return(NULL);
		} else
			post("Kinect with serial %s opened!", (char*)serial->s_name);
	}

	freenect_set_user(f_dev, x);

	freenect_set_audio_in_callback(f_dev, in_callback);
	
	if (freenect_start_audio(f_dev) < 0)
		post("Couldn't start audio transfer\n");
	
	// RESERVE BUFFERS AND SET ZERO
	x->x_bufsize = FREENECT_BUFFER*16; // 16 kHz Samplingrate Kinect
	
	x->x_buffer1 = (float*)getbytes(x->x_bufsize*sizeof(float));
	memset(x->x_buffer1, 0, x->x_bufsize*sizeof(float));
	x->x_buffer2 = (float*)getbytes(x->x_bufsize*sizeof(float));
	memset(x->x_buffer2, 0, x->x_bufsize*sizeof(float));
	x->x_buffer3 = (float*)getbytes(x->x_bufsize*sizeof(float));
	memset(x->x_buffer3, 0, x->x_bufsize*sizeof(float));
	x->x_buffer4 = (float*)getbytes(x->x_bufsize*sizeof(float));
	memset(x->x_buffer4, 0, x->x_bufsize*sizeof(float));
	
	// INIT POSITIONS
	x->x_freenect_pos = 0;
	x->x_playback_pos = 0;
	x->x_ready = 0;
	x->AverageSRate=0;
	x->EstimatedSRate=0;
	
	x->exit_thread = 0;
		
	// INIT MUTEX
	pthread_mutex_init(&x->x_mutex, 0);
	
	int res=pthread_create(&x->x_freenect_thread, NULL, freenect_thread_func,x);
	if (res)
	{
	  post("pthread_func failed");
    }
  
	// Generate Outlets
  x->x_out1=outlet_new(&x->x_obj, NULL);
  x->x_out2=outlet_new(&x->x_obj, NULL);
  x->x_out3=outlet_new(&x->x_obj, NULL);
  x->x_out4=outlet_new(&x->x_obj, NULL);
  x->x_infoout=outlet_new(&x->x_obj, NULL);

  return (x);
}

void freenect_audio_setup(void){

  freenect_audio_class = class_new(gensym("freenect_audio"), 
		(t_newmethod)freenect_audio_new, 
			0, sizeof(t_freenect_audio), 
			CLASS_DEFAULT,
			A_GIMME, 0);

	class_addbang(freenect_audio_class, (t_method)freenect_audio_bang);
	class_addmethod(freenect_audio_class, (t_method)freenect_audio_info, gensym("info"), 0);
}
