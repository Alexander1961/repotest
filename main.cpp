#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <pthread.h>
#include <time.h>

//----------- the base ----------------------------

struct Frame { void *data; size_t size; };

void millisleep(int ms)
{
#ifdef _WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

bool produce_frame(struct Frame* pFrame)
{
    static char data[16] = {"123456789ABCDEF"};
	static int cnt = 0;  

	// produce different frames, and 3 duplicated ones each 16 frames
	if ((cnt & 0x0f) > 2)
		data[0] = (char)('0' + cnt % 10); 

    pFrame->data = data;
    pFrame->size = 16;
	cnt++;
    millisleep(20); // sleep 20 ms for 50 fps
    return true;
}

// It's not clear from the assignment how to detect if the producer produces duplicated frame.
// To simulate the repeated frame condition I compare only the first byte of data.
bool same_frame(const struct Frame* frame1, const struct Frame* frame2)
{
	return ((const char*)frame1->data)[0] == ((const char*)frame2->data)[0];
}

static int consumer_delay = 15;
void consume_frame(struct Frame frame)
{
    //here we should do something with the frame
    millisleep(consumer_delay);
}
//-------------- The capture manager --------------------
// queue size exponent to have ring buffer for 7 frames
#define FQ_EXP  3
// frame queue size
#define FQ_SIZE (1 << FQ_EXP)
// frame ring buffer index mask
#define FQ_MASK (FQ_SIZE - 1)

/**
 * @brief Capture Manager struct
 */
struct CapMng
{
    // ring buffer for frames
    struct Frame frames[FQ_SIZE];		// stores data pointers
	struct Frame framebuffers[FQ_SIZE]; // stores producer data in allocated buffers;
    unsigned int idx_in, idx_out;
	int current_buffer;		// last used buffer index, initially -1 if buffers never used.

    // condition to signal when the new frame is captured
    pthread_mutex_t mutex;
    pthread_cond_t cond;  // new event

    // frame counters - just for fun;
    int nFramesProduced, nFramesConsumed, nFramesRepeated;

    volatile bool stop; //!< flag to stop threads
};

/* The following functions are not mutex protected and may suffer data race
but it's not an issue because data race would only lead to 'over-protection' in our case.
*/
inline bool rbuf_empty(struct CapMng* pCapMng)
{
    return (pCapMng->idx_in == pCapMng->idx_out);
}

inline bool rbuf_full(struct CapMng* pCapMng)
{
    return ((pCapMng->idx_in + 1) & FQ_MASK) == pCapMng->idx_out;
}

inline unsigned rbuf_queuesize(struct CapMng* pCapMng)
{
	return (pCapMng->idx_in - pCapMng->idx_out) & FQ_MASK;
}

/**
 * @brief push_frame Push one captured frame for consumer
 * @param pCapMng Capture manager
 * @param frame The frame produced
 * @return true on success, false if buffer full.
 */
bool push_frame(struct CapMng* pCapMng, struct Frame frame)
{
	struct Frame* pCurBuf = NULL;

    if (rbuf_full(pCapMng))
        return false;  // buffer full - new frames not accepted

	bool bRepFrame = false;
	if (pCapMng->current_buffer >= 0)
	{
		// test repeated frame condition
		pCurBuf = &pCapMng->framebuffers[pCapMng->current_buffer];
		bRepFrame = same_frame(pCurBuf, &frame);
		if (bRepFrame)
			pCapMng->nFramesRepeated++;
	}

	if (bRepFrame == false)
	{
		// allocate space if necessary and copy the frame to the correspondent buffer.
		pCapMng->current_buffer = (pCapMng->current_buffer + 1) & FQ_MASK;
		pCurBuf = &pCapMng->framebuffers[pCapMng->current_buffer];
		if (pCurBuf->data == NULL)
			pCurBuf->data = malloc(frame.size);  // assume the size is always the same
		
		memcpy(pCurBuf->data, frame.data, frame.size);
		pCurBuf->size = frame.size;
	}

	bool bEmpty = rbuf_empty(pCapMng);
    // put the new frame to the queue; we put only data ptr which is the same if the frame is repeated
	pCapMng->frames[pCapMng->idx_in] = *pCurBuf;  // here pCurBuf is never NULL
	pCapMng->idx_in = (pCapMng->idx_in + 1) & FQ_MASK;

	// signal the condition if the buffer was empty.
	// Race condition may occur if the frame is consumed just before signaling the condition
	// So, we've put a protection in the consumer for this case.
	if (bEmpty)
	{
		pthread_mutex_lock(&pCapMng->mutex);
		pthread_cond_signal(&pCapMng->cond);
		pthread_mutex_unlock(&pCapMng->mutex);
	}

    return rbuf_queuesize(pCapMng) <= FQ_SIZE / 2;
}

// ------- Producer thread ----------

void *producer(void* p)
{
    struct CapMng* pCapMng = (struct CapMng*)p;
	struct Frame frame = { NULL, 0 };
	bool bSkipFrame = false;

    // run the producer
    while(!pCapMng->stop)
    {
        if (produce_frame(&frame))
        {
            pCapMng->nFramesProduced++;
            // if the manager indicates to slow down just drop one frame
            if (bSkipFrame)
				bSkipFrame = false;
			else
                bSkipFrame = !push_frame(pCapMng, frame);
        }
    }

    return NULL;
}

// -------- Consumer thread -----------
void *consumer(void *p)
{
    struct CapMng* pCapMng = (struct CapMng*)p;
	struct Frame frame;

	// run the consumer loop
    while(!pCapMng->stop)
    {
		// If no pending frames, wait for "frame produced" condition
		// otherwise, consume immediately
		frame.data = NULL;
		if (rbuf_empty(pCapMng))  
		{
			pthread_mutex_lock(&pCapMng->mutex);
			pthread_cond_wait(&pCapMng->cond, &pCapMng->mutex);
			if (!rbuf_empty(pCapMng))  // protection
			{
				frame = pCapMng->frames[pCapMng->idx_out];
				pCapMng->idx_out = (pCapMng->idx_out + 1) & FQ_MASK;
			}
			pthread_mutex_unlock(&pCapMng->mutex);
		}
		else
		{
			frame = pCapMng->frames[pCapMng->idx_out];
			pCapMng->idx_out = (pCapMng->idx_out + 1) & FQ_MASK;
		}

		if (frame.data != NULL)
		{
			consume_frame(frame);
			pCapMng->nFramesConsumed++;
		}
     }

    return NULL;
}
//--------------------------- main ----------------------------------------

int main(int argc, const char *argv[])
{
    struct CapMng capMng;
    pthread_t th_producer, th_consumer;
    int  nRet = 0;
    void *pRet = NULL;

	// usage
	printf("USE: frames [consumer delay in milliseconds]\nProducer will produce 1 frame each 20 ms\n");
	if (argc > 1)
	{
		consumer_delay = atoi(argv[1]);
		if (consumer_delay < 1 || consumer_delay >= 200)
		{
			printf("Bad consumer delay = %d ms, should be between 1 and 200; set to 15\n", consumer_delay);
			consumer_delay = 15;
		}
	}
    // initialize the manager
    capMng.idx_in = capMng.idx_out = 0;
    capMng.nFramesConsumed = capMng.nFramesProduced = capMng.nFramesRepeated = 0;
    capMng.stop = false;
	capMng.current_buffer = -1;
	for (nRet = 0; nRet < FQ_SIZE; nRet++)
		capMng.framebuffers[nRet].data = NULL;  // initialize to NULL to allocate later

    pthread_mutex_init(&capMng.mutex,NULL);
    pthread_cond_init(&capMng.cond,NULL);

    // start threads
	printf("Consumer will last %d ms to consume one frame\n", consumer_delay);
    printf("Starting threads...\n");
    nRet = pthread_create(&th_producer, NULL, producer, (void *)&capMng);
    if (nRet != 0)
    {
         printf("ERROR %d starting producer\n", nRet);
         exit(-1);
    }

    nRet = pthread_create(&th_consumer, NULL, consumer, (void *)&capMng);
    if (nRet != 0)
    {
         printf("ERROR %d starting consumer\n", nRet);
         exit(-1);
    }

    // dummy loop: wait 10 seconds and exit
    for(nRet = 0; nRet < 10; nRet++)
    {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        printf("Time = %ds, frames produced = %d, consumed = %d, repeated = %d, queue size = %u\n",
               nRet + 1, capMng.nFramesProduced, capMng.nFramesConsumed, capMng.nFramesRepeated
               , rbuf_queuesize(&capMng));
    }

    // stop the threads and exit
    capMng.stop = true;
    pthread_mutex_lock(&capMng.mutex);
    pthread_cond_signal(&capMng.cond);  // unlock the consumer if it's waiting
    pthread_mutex_unlock(&capMng.mutex);

    pthread_join(th_producer, &pRet);
    pthread_join(th_consumer, &pRet);
	
	// buffer cleanup
	for (nRet = 0; nRet < FQ_SIZE; nRet++)
		free(capMng.framebuffers[nRet].data); 

	printf("That's all folks!\n");

    return 0;
}

