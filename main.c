#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <time.h>

//camera/mmal/raspberry specific libraries
#include "bcm_host.h"
#include "mmal.h"
#include "util/mmal_default_components.h"
#include "util/mmal_util.h"
#include "util/mmal_connection.h"
#include "util/mmal_util_params.h"
#include "interface/vcos/vcos.h"

#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

//will affect framerate, it seems that if framerate is higher than possible shutter speed, it will be automatically lowered
#define CAMERA_SHUTTER_SPEED 15000

//framerate above 30 only possible for some resolution, depends on the camera
//can also reduce the displayed portion of the camera on screen
#define CAMERA_FRAMERATE 30

//resolution needs to be smaller than the screen size
#define CAMERA_RESOLUTION_X 1280
#define CAMERA_RESOLUTION_Y 720

#define CHECK_STATUS(status, msg) if (status != MMAL_SUCCESS) { fprintf(stderr, msg"\n\r");}

char *fbp;
uint32_t screen_size_x = 0;
uint32_t screen_size_y = 0;

static int cur_sec;

sem_t semaphore;

void framebuffer_init();
void output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);

void init_time_keeping();
float get_cur_time();

void main(void){
    //sets up the framebuffer, will draw
    framebuffer_init();

    MMAL_STATUS_T status = MMAL_EINVAL;
    MMAL_COMPONENT_T *camera;
    MMAL_PORT_T *video_port;
    MMAL_ES_FORMAT_T *format;
    MMAL_POOL_T *pool;

    sem_init(&semaphore, 0, 0);

    bcm_host_init();

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
    CHECK_STATUS(status, "failed to create decoder");

    status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_SHUTTER_SPEED, CAMERA_SHUTTER_SPEED);
    CHECK_STATUS(status, "failed to set shutter speed");

    video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];

    format = video_port->format;
    format->encoding = MMAL_ENCODING_RGB24;
    format->es->video.width = VCOS_ALIGN_UP(CAMERA_RESOLUTION_X, 32);
    format->es->video.height = VCOS_ALIGN_UP(CAMERA_RESOLUTION_Y, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = CAMERA_RESOLUTION_X;
    format->es->video.crop.height = CAMERA_RESOLUTION_Y;

    printf("Camera: resolution %dx%d\n\r", CAMERA_RESOLUTION_X, CAMERA_RESOLUTION_Y);

    status = mmal_port_format_commit(video_port);
    CHECK_STATUS(status, "failed to commit format");

    //second paramter of the second parameter is the denominator for the framerate
    MMAL_PARAMETER_FRAME_RATE_T framerate_param = {{MMAL_PARAMETER_VIDEO_FRAME_RATE, sizeof(framerate_param)}, {CAMERA_FRAMERATE, 0}};
    status = mmal_port_parameter_set(video_port, &framerate_param.hdr);
    CHECK_STATUS(status, "failed to set framerate");

    //two buffers seem a good compromise, more will cause some latency
    video_port->buffer_num = 3;
    pool = mmal_port_pool_create(video_port, video_port->buffer_num, video_port->buffer_size);

    video_port->userdata = (void *)pool->queue;

    status = mmal_component_enable(camera);
    CHECK_STATUS(status, "failed to enable camera");

    //will call the callback function everytime there is an image available
    status = mmal_port_enable(video_port, output_callback);
    CHECK_STATUS(status, "failed to enable video port");

    usleep(250);

    //necessary parameter to get the RGB data out of the video port
    status = mmal_port_parameter_set_boolean(video_port, MMAL_PARAMETER_CAPTURE, 1);
    CHECK_STATUS(status, "failed to set parameter capture");

    //need to provide the buffers to the port
    int queue_length = mmal_queue_length(pool->queue);
    for(int i = 0; i < queue_length; i++){
        MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(pool->queue);
        if(buffer == NULL){
            printf("problem to get the buffer\n\r");
        }

        status = mmal_port_send_buffer(video_port, buffer);
        CHECK_STATUS(status, "could not send buffer");
    }

    MMAL_BUFFER_HEADER_T *buffer;
    float time_since_report = 0.0f;
    int count_frames = 0;

    float start_time;
    float end_time;
    float start_copy_time;
    float end_copy_time;

    init_time_keeping();

    while(1){
        start_time = get_cur_time();

        //wait until a buffer has been received
        sem_wait(&semaphore);

        buffer = mmal_queue_get(pool->queue);

        start_copy_time = get_cur_time();

        //draw the image on the top left corner of the framebuffer
        //would be less costly to limit frambuffer size and just do a memcpy
        int offset_data = 0;
        for(int i = 0; i < CAMERA_RESOLUTION_Y; i++){
           for(int j = 0; j < CAMERA_RESOLUTION_X*4; j+=4){
               int idx = i*screen_size_x*4+j;
               //seem that R and B components are inverted
               fbp[idx] = buffer->data[offset_data+2];
               fbp[idx+1] = buffer->data[offset_data+1];
               fbp[idx+2] = buffer->data[offset_data+0];
               fbp[idx+3] = 0;
               offset_data += 3;
           }
        }

        end_copy_time = get_cur_time();
        // printf("frame copy time: %f\n\r", end_copy_time-start_copy_time);

        //Send back the buffer to the port to be filled with an image again
        mmal_buffer_header_release(buffer);
        mmal_port_send_buffer(video_port, buffer);

        end_time = get_cur_time();
        float seconds = (float)(end_time - start_time);
        time_since_report += seconds;
        count_frames++;

        if(time_since_report > 1.0f){
            float framerate = count_frames/time_since_report;
            printf("frequency: %fHz\n\r", framerate);
            time_since_report = 0;
            count_frames = 0;
        }
    }

    //todo free the mmal and framebuffer ressources cleanly
}

void framebuffer_init(){
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    int fb_d = open("/dev/fb0", O_RDWR);
    ioctl(fb_d, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fb_d, FBIOGET_VSCREENINFO, &vinfo);

    printf("Framebuffer: resolution %dx%d with %dbpp\n\r", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    screen_size_x = vinfo.xres;
    screen_size_y = vinfo.yres;

    fbp = (char*)mmap(0, screen_size_x*screen_size_y*4, PROT_READ | PROT_WRITE, MAP_SHARED, fb_d, 0);
    //draw a gradient background
    for(int i = 0; i < screen_size_y; i++){
        for(int j = 0; j < screen_size_x*4; j+=4){
            int idx = i*screen_size_x*4+j;
            fbp[idx] = (i*255)/screen_size_y;
            fbp[idx+1] = (j*255)/(screen_size_x*4);
            fbp[idx+2] = 128;
            fbp[idx+3] = 0;
        }
    }
}

void output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer){
   struct MMAL_QUEUE_T *queue = (struct MMAL_QUEUE_T *)port->userdata;

   mmal_queue_put(queue, buffer);

   sem_post(&semaphore);

}

//clock_gettime is a better time keeping mechanism than other on the raspberry pi
void init_time_keeping(){
    struct timespec time_read;
    clock_gettime(CLOCK_REALTIME, &time_read);
    cur_sec = time_read.tv_sec; //global
}

float get_cur_time(){
    struct timespec time_read;
    clock_gettime(CLOCK_REALTIME, &time_read);
    return (time_read.tv_sec-cur_sec)+time_read.tv_nsec/1000000000.0f;
}
