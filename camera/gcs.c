//#include <stddef.h>
#include "gcs.h"

#include "interface/vcos/vcos_stdbool.h"
#include "interface/vcos/vcos_inttypes.h"

#include <libcamera/libcamera.h>

/* Watchdog timeout - elapsed time to allow for no video frames received */
#define GCS_WATCHDOG_TIMEOUT_MS  4000

/* How many buffers the camera has to work with. 
 * 3 minimum, but might introduce some latency as only 2 can be used alternatingly in the background while one processes.
 * More than 4 are not needed and not used. */
#define GCS_SIMUL_BUFFERS 4

/* GPU Camera Stream
	Simple MMAL camera stream using the preview port, keeping only the most recent camera frame buffer for realtime, low-latency CV applications
	Handles MMAL component creation and setup

	Watchdog: Watches and stops stream if frames have stopped coming. Implemented by a timeout since last frame
	Buffer Pool: Collection of buffers used by the camera output to write to, processed and dropped frames are returned to it
*/
struct GCS
{
	// Flags
	uint8_t started; // Specifies that stream have started
	uint8_t error;

	// Camera parameters
	GCS_CameraParams cameraParams;

	std::shared_ptr<Camera> camera;; // Camera component
	std::unique_ptr<CameraConfiguration> config; // Camera output port (preview)
	FrameBufferAllocator *allocator; // Pool of buffers for camera output to use
	std::unique_ptr<Request> request; // Nost recent camera frame buffer
	std::set<CompletedRequest *> completed_request; // Nost recent camera frame buffer
	VCOS_TIMER_T watchdogTimer; // Watchdog to detect if camera stops sending frames
	VCOS_MUTEX_T frameReadyMutex; // For waiting for next ready frame
};

/* Local functions (callbacks) */

// Camera control callback that receives events (incl. errors) of the MMAL camera
static void gcs_onCameraControl(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
// Camera output callback that receives new camera frames and updates the current frame
static void gcs_onCameraOutput(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
// Watchdog callback for when no new camera frames are pushed for a while, indicating an error
static void gcs_onWatchdogTrigger(void *context);


GCS *gcs_create(GCS_CameraParams *cameraParams)
{
	// Temporary status return values
	//MMAL_STATUS_T mstatus;
	//VCOS_STATUS_T vstatus;

	//LOG_TRACE("Creating GPU Camera Stream");

	// Allocate memory for structure
	GCS *gcs;
	//CHECK_STATUS_V((gcs ? VCOS_SUCCESS : VCOS_ENOMEM), "Failed to allocate context", error_allocate);
	gcs->cameraParams = *cameraParams;

	// Access mutex
	//vstatus = vcos_mutex_create(&gcs->frameReadyMutex, "gcs-mutex");
	//CHECK_STATUS_V(vstatus, "Failed to create mutex", error_mutex);

	// Setup timers and callbacks for watchdog (resets whenever a frame is received)
	//vstatus = vcos_timer_create(&gcs->watchdogTimer, "gcs-watchdog-timer", gcs_onWatchdogTrigger, gcs);
	//CHECK_STATUS_V(vstatus, "Failed to create timer", error_timer);

	// Create MMAL camera component
	std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
	cm->start();
	
	if (cm->cameras().empty()) {
		std::cout << "No cameras were identified on the system."
			  << std::endl;
		cm->stop();
		return EXIT_FAILURE;
	}
	
	std::string cameraId = cm->cameras()[gcs->cameraParams.camera_num]->id();
	camera = cm->get(cameraId);
	camera->acquire();
	
	std::unique_ptr<CameraConfiguration> config =
	camera->generateConfiguration( { StreamRole::Viewfinder } );

	Size size(1920, 1080);
	config->at(0).pixelFormat = libcamera::formats::YUV420;
	config->at(0).size = size;
	config->at(0).stride = 1344;
	
	StreamConfiguration &streamConfig = config->at(0);
	std::cout << "Default viewfinder configuration is: " << streamConfig.toString() << std::endl;
	
	config->validate();
	std::cout << "Validated viewfinder configuration is: " << streamConfig.toString() << std::endl;
	
	camera->configure(config.get());
	
	FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

	for (StreamConfiguration &cfg : *config) {
		int ret = allocator->allocate(cfg.stream());
		if (ret < 0) {
			std::cerr << "Can't allocate buffers" << std::endl;
			return EXIT_FAILURE;
		}

		size_t allocated = allocator->buffers(cfg.stream()).size();
		std::cout << "Allocated " << allocated << " buffers for stream" << std::endl;
	}
	
	Stream *stream = streamConfig.stream();
	const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
	std::vector<std::unique_ptr<Request>> requests;
	for (unsigned int i = 0; i < buffers.size(); ++i) {
		std::unique_ptr<Request> request = camera->createRequest();
		if (!request)
		{
			std::cerr << "Can't create request" << std::endl;
			return EXIT_FAILURE;
		}

		const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
		int ret = request->addBuffer(stream, buffer.get());
		if (ret < 0)
		{
			std::cerr << "Can't set buffer for request"
				  << std::endl;
			return EXIT_FAILURE;
		}

		/*
		 * Controls can be added to a request on a per frame basis.
		 */
		ControlList &controls = request->controls();
		controls.set(controls::Brightness, 0.5);

		requests.push_back(std::move(request));
	}
	
	camera->requestCompleted.connect(requestComplete);

	LOG_TRACE("Finished setup of GCS");

	return gcs;
}

void gcs_destroy(GCS *gcs)
{
	if (!gcs) return;

	// Stop worker thread, disable camera component
	gcs_stop(gcs);

	// Destroy camera component
	camera->stop();
	camera->release();

	// Free remaining resources
	allocator->free(stream);
	delete allocator;
	vcos_mutex_delete(&gcs->frameReadyMutex);
	vcos_timer_delete(&gcs->watchdogTimer);
	vcos_free(gcs);
	
	cm->stop();
}

/* Start GCS (camera stream). Enables MMAL camera and starts watchdog */
uint8_t gcs_start(GCS *gcs)
{
	camera->start();
	
	for (std::unique_ptr<Request> &request : requests)
		camera->queueRequest(request.get());

	return 0;

error_port:
	return -1;
}

/* Stop GCS (camera output). Stops watchdog and disabled MMAL camera */
void gcs_stop(GCS *gcs)
{
	gcs->started = 0;

	// Stop running timers
	vcos_timer_cancel(&gcs->watchdogTimer);

	if (gcs->started)
	{
		// Disable camera output
		mmal_port_disable(gcs->cameraOutput);

		// Stop potentially waiting user
		if (vcos_mutex_is_locked(&gcs->frameReadyMutex))
			vcos_mutex_unlock(&gcs->frameReadyMutex);

		// Reset unused frames
		if (gcs->processingFrameBuffer)
		{
			mmal_buffer_header_release(gcs->processingFrameBuffer);
			gcs->processingFrameBuffer = NULL;
		}
		if (gcs->curFrameBuffer)
		{
			mmal_buffer_header_release(gcs->curFrameBuffer);
			gcs->curFrameBuffer = NULL;
		}
	}
}

/* Returns whether there is a new camera frame available */
uint8_t gcs_hasFrameBuffer(GCS *gcs)
{
	return gcs->curFrameBuffer != NULL;
}

/* Returns the most recent camera frame. If no camera frame is available yet, blocks until there is.
 * If the last frame has not been returned yet, returns NULL. */
void* gcs_requestFrameBuffer(GCS *gcs)
{
	vcos_mutex_lock(&gcs->frameReadyMutex);
	if (gcs->processingFrameBuffer)
	{ // Not cleaned up last frame
		LOG_ERROR("Not cleaned up last fraame!");
		return NULL;
	}
	gcs->processingFrameBuffer = gcs->curFrameBuffer;
	gcs->curFrameBuffer = NULL;
	if (!gcs->processingFrameBuffer)
	{ // Not cleaned up last frame
		LOG_ERROR("No current frame buffer!");
		return NULL;
	}
	return gcs->processingFrameBuffer;
}

/* Returns the data of the given MMAL framebuffer. Use after gcs_requestFrameBuffer to get the underlying buffer. */
void* gcs_getFrameBufferData(void *framebuffer)
{
	return ((MMAL_BUFFER_HEADER_T*)framebuffer)->data;
}

/* Return requested Frane Buffer after processing is done.
 * Has to be called before a new frame buffer can be requested. */
void gcs_returnFrameBuffer(GCS *gcs)
{
	if (gcs->processingFrameBuffer)
	{
		mmal_buffer_header_release(gcs->processingFrameBuffer);
		gcs->processingFrameBuffer = NULL;
	}
}

/** Callback from the camera control port. */
static void gcs_onCameraControl(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
	GCS *gcs = (GCS *)port->userdata;
	if (buf->cmd == MMAL_EVENT_ERROR)
	{
		LOG_ERROR("%s: MMAL error: %s", port->name, mmal_status_to_string(*(MMAL_STATUS_T *)buf->data));
		gcs_stop(gcs);
	}
	else
	{
		LOG_TRACE("%s: buf %p, event %4.4s", port->name, buf, (char *)&buf->cmd);
	}
	mmal_buffer_header_release(buf);
}

/** Callback from camera output port - receive buffer and replace as current */
static void gcs_onCameraOutput(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	GCS *gcs = (GCS *)port->userdata;
	if (buffer->length == 0)
	{
		LOG_TRACE("%s: zero-length buffer => EOS", port->name);
		mmal_buffer_header_release(buffer);
	}
	else if (buffer->data == NULL)
	{
		LOG_ERROR("%s: zero buffer handle", port->name);
		mmal_buffer_header_release(buffer);
	}
	else if (!gcs->started)
	{
		mmal_buffer_header_release(buffer);
	}
	else
	{
		// Reset watchdog timer for detecting when frames stop coming
		vcos_timer_set(&gcs->watchdogTimer, GCS_WATCHDOG_TIMEOUT_MS);

		// Retract frame ready signal during switch
		vcos_mutex_trylock(&gcs->frameReadyMutex);

		// Release now outdated camera frame
		if (gcs->curFrameBuffer != NULL)
		{ // If it does not exist, it has been consumed
			mmal_buffer_header_release(gcs->curFrameBuffer);
			gcs->curFrameBuffer = NULL;
			// On drop frame
		}

		// Set the newest camera frame
		gcs->curFrameBuffer = buffer;

		// Send buffer back to port for use (needed? it's a port buffer, should automatically do it, right?)
		while ((buffer = mmal_queue_get(gcs->bufferPool->queue)) != NULL)
		{
			MMAL_STATUS_T status = mmal_port_send_buffer(gcs->cameraOutput, buffer);
			if (status != MMAL_SUCCESS)
				LOG_ERROR("Failed to send buffer to %s", gcs->cameraOutput->name);
		}
	}

	// If not done already signal that a frame is ready
	if (vcos_mutex_is_locked(&gcs->frameReadyMutex))
		vcos_mutex_unlock(&gcs->frameReadyMutex);
}

/** Watchdog timer callback - stops playback because no frames have arrived from the camera for a while */
static void gcs_onWatchdogTrigger(void *context)
{
	GCS *gcs = context;
	LOG_ERROR("%s: no frames received for %d ms, aborting", gcs->cameraOutput->name, GCS_WATCHDOG_TIMEOUT_MS);
	gcs_stop(gcs);
}

int gcs_annotate(GCS *gcs, const char *string) 
{
	//Annotate text (work in progess)
    MMAL_PARAMETER_CAMERA_ANNOTATE_V4_T annotate =
    {{MMAL_PARAMETER_ANNOTATE, sizeof(MMAL_PARAMETER_CAMERA_ANNOTATE_V4_T)}};
    
    //const char *string = "this is just a test";
    //annotate.string = string;
    //time_t t = time(NULL);
    //struct tm tm = *localtime(&t);
    //char tmp[MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3];
    //int process_datetime = 1;

    annotate.enable = 1;
    annotate.enable_text_background = MMAL_TRUE;
    //annotate.text_size = 64;
    //annotate.show_frame_num = MMAL_TRUE;
    
    strncpy(annotate.text, string, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3);
    
    //set the text color to apepar white on the blue background (also works for a black background)
    annotate.custom_text_colour = MMAL_TRUE;
    annotate.custom_text_Y = 255;
    annotate.custom_text_U = 255;
    annotate.custom_text_V = 107;
    
    //create a blue background for the text
    annotate.custom_background_colour = MMAL_TRUE;
    annotate.custom_background_Y = 29;  //0   (for black)
    annotate.custom_background_U = 255; //128 (for black)
    annotate.custom_background_V = 107; //128 (for black)
         
    return mmal_port_parameter_set(gcs->camera->control, &annotate.hdr);
	
}
