#include "kvm/threadpool.h"
#include "kvm/mutex.h"
#include "kvm/kvm.h"

#include <linux/kernel.h>
#include <linux/list.h>
#include <pthread.h>
#include <stdbool.h>

static pthread_mutex_t	job_mutex	= PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t	thread_mutex	= PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	job_cond	= PTHREAD_COND_INITIALIZER;

static LIST_HEAD(head);

static pthread_t	*threads;
static long		threadcount;
static bool		running;

static struct thread_pool__job *thread_pool__job_pop_locked(void)
{
	struct thread_pool__job *job;

	if (list_empty(&head))
		return NULL;

	job = list_first_entry(&head, struct thread_pool__job, queue);
	list_del(&job->queue);

	return job;
}

static void thread_pool__job_push_locked(struct thread_pool__job *job)
{
	list_add_tail(&job->queue, &head);
}

static struct thread_pool__job *thread_pool__job_pop(void)
{
	struct thread_pool__job *job;

	mutex_lock(&job_mutex);
	job = thread_pool__job_pop_locked();
	mutex_unlock(&job_mutex);
	return job;
}

static void thread_pool__job_push(struct thread_pool__job *job)
{
	mutex_lock(&job_mutex);
	thread_pool__job_push_locked(job);
	mutex_unlock(&job_mutex);
}

static void thread_pool__handle_job(struct thread_pool__job *job)
{
	while (job) {
		job->callback(job->kvm, job->data);

		mutex_lock(&job->mutex);

		if (--job->signalcount > 0)
			/* If the job was signaled again while we were working */
			thread_pool__job_push(job);

		mutex_unlock(&job->mutex);

		job = thread_pool__job_pop();
	}
}

static void thread_pool__threadfunc_cleanup(void *param)
{
	mutex_unlock(&job_mutex);
}

static void *thread_pool__threadfunc(void *param)
{
	pthread_cleanup_push(thread_pool__threadfunc_cleanup, NULL);

	while (running) {
		struct thread_pool__job *curjob = NULL;

		mutex_lock(&job_mutex);
		while (running && (curjob = thread_pool__job_pop_locked()) == NULL)
			pthread_cond_wait(&job_cond, &job_mutex);
		mutex_unlock(&job_mutex);

		if (running)
			thread_pool__handle_job(curjob);
	}

	pthread_cleanup_pop(0);

	return NULL;
}

static int thread_pool__addthread(void)
{
	int res;
	void *newthreads;

	mutex_lock(&thread_mutex);
	newthreads = realloc(threads, (threadcount + 1) * sizeof(pthread_t));
	if (newthreads == NULL) {
		mutex_unlock(&thread_mutex);
		return -1;
	}

	threads = newthreads;

	res = pthread_create(threads + threadcount, NULL,
			     thread_pool__threadfunc, NULL);

	if (res == 0)
		threadcount++;
	mutex_unlock(&thread_mutex);

	return res;
}

int thread_pool__init(struct kvm *kvm)
{
	unsigned long i;
	unsigned int thread_count = sysconf(_SC_NPROCESSORS_ONLN);

	running = true;

	for (i = 0; i < thread_count; i++)
		if (thread_pool__addthread() < 0)
			return i;

	return i;
}
late_init(thread_pool__init);

int thread_pool__exit(struct kvm *kvm)
{
	int i;
	void *NUL = NULL;

	running = false;

	for (i = 0; i < threadcount; i++) {
		mutex_lock(&job_mutex);
		pthread_cond_signal(&job_cond);
		mutex_unlock(&job_mutex);
	}

	for (i = 0; i < threadcount; i++) {
		pthread_join(threads[i], NUL);
	}

	return 0;
}
late_exit(thread_pool__exit);

void thread_pool__do_job(struct thread_pool__job *job)
{
	struct thread_pool__job *jobinfo = job;

	if (jobinfo == NULL || jobinfo->callback == NULL)
		return;

	mutex_lock(&jobinfo->mutex);
	if (jobinfo->signalcount++ == 0)
		thread_pool__job_push(job);
	mutex_unlock(&jobinfo->mutex);

	mutex_lock(&job_mutex);
	pthread_cond_signal(&job_cond);
	mutex_unlock(&job_mutex);
}
