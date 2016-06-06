/*
   mini-LZ library (mlz)
   (c) Martin Sedlak 2016

   Boost Software License - Version 1.0 - August 17th, 2003

   Permission is hereby granted, free of charge, to any person or organization
   obtaining a copy of the software and accompanying documentation covered by
   this license (the "Software") to use, reproduce, display, distribute,
   execute, and transmit the Software, and to prepare derivative works of the
   Software, and to permit third-parties to whom the Software is furnished to
   do so, all subject to the following:

   The copyright notices in the Software and this entire statement, including
   the above license grant, this restriction and the following disclaimer,
   must be included in all copies of the Software, in whole or in part, and
   all derivative works of the Software, unless such copies or derivative
   works are solely in the form of machine-executable object code generated by
   a source language processor.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
   SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
   FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

#include "mlz_thread.h"
#include "mlz_enc.h"

#if defined(MLZ_THREADS)

#include <string.h>

#if defined(_WIN32)

#	if !defined(WIN32_LEAN_AND_MEAN)
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#	include <process.h>

mlz_mutex mlz_mutex_create(void)
{
	LPCRITICAL_SECTION csec = (LPCRITICAL_SECTION)mlz_malloc(sizeof(CRITICAL_SECTION));
	MLZ_RET_FALSE(csec);
	InitializeCriticalSection(csec);
	return (mlz_mutex)csec;
}

mlz_bool mlz_mutex_destroy(mlz_mutex mutex)
{
	MLZ_RET_FALSE(mutex);
	DeleteCriticalSection((LPCRITICAL_SECTION)mutex);
	mlz_free(mutex);
	return MLZ_TRUE;
}

mlz_bool mlz_mutex_lock(mlz_mutex mutex)
{
	MLZ_RET_FALSE(mutex);
	EnterCriticalSection((LPCRITICAL_SECTION)mutex);
	return MLZ_TRUE;
}

mlz_bool mlz_mutex_unlock(mlz_mutex mutex)
{
	MLZ_RET_FALSE(mutex);
	LeaveCriticalSection((LPCRITICAL_SECTION)mutex);
	return MLZ_TRUE;
}

mlz_event mlz_event_create(void)
{
	return (mlz_event)CreateEvent(MLZ_NULL, TRUE, FALSE, MLZ_NULL);
}

mlz_bool mlz_event_destroy(mlz_event event)
{
	return CloseHandle((HANDLE)event) != FALSE;
}

mlz_bool mlz_event_reset(mlz_event event)
{
	return ResetEvent((HANDLE)event) != FALSE;
}

mlz_bool mlz_event_set(mlz_event event)
{
	return SetEvent((HANDLE)event) != FALSE;
}

mlz_bool mlz_event_wait(mlz_event event)
{
	return WaitForSingleObject((HANDLE)event, INFINITE) == WAIT_OBJECT_0;
}

mlz_thread mlz_thread_create(void)
{
	mlz_thread res = (mlz_thread)mlz_malloc(sizeof(struct mlz_thread));
	if (res) {
		res->handle = MLZ_NULL;
		res->proc   = MLZ_NULL;
		res->param  = MLZ_NULL;
	}
	return res;
}

/* FIXME: in 64-bit mode, clang warns about __stdcall (ignored attribute) */
/* unfortunately using CALLBACK won't help here either                    */
static unsigned __stdcall mlz_win_thread_proc(void *param)
{
	mlz_thread thread = (mlz_thread)param;
	thread->proc(thread->param);
	return 0;
}

mlz_bool mlz_thread_run(
	mlz_thread      thread,
	mlz_thread_proc proc,
	void           *param
)
{
	uintptr_t handle;
	MLZ_RET_FALSE(thread && !thread->handle && proc);
	thread->proc   = proc;
	thread->param  = param;
	handle = _beginthreadex(MLZ_NULL, 0, mlz_win_thread_proc, thread, 0, MLZ_NULL);
	MLZ_RET_FALSE(handle);
	thread->handle = (void *)handle;
	return MLZ_TRUE;
}

mlz_bool mlz_thread_destroy(mlz_thread thread)
{
	MLZ_RET_FALSE(thread);
	if (thread->handle)
		MLZ_RET_FALSE(CloseHandle((HANDLE)thread->handle) != FALSE);
	thread->handle = MLZ_NULL;
	mlz_free(thread);
	return MLZ_TRUE;
}

mlz_bool mlz_thread_join(mlz_thread thread)
{
	MLZ_RET_FALSE(thread);
	return WaitForSingleObject((HANDLE)thread->handle, INFINITE) == WAIT_OBJECT_0;
}

/* !Windows */
#else

#	include <pthread.h>

mlz_mutex mlz_mutex_create()
{
	pthread_mutex_t *res = (pthread_mutex_t *)mlz_malloc(sizeof(pthread_mutex_t));
	MLZ_RET_FALSE(res);

	if (pthread_mutex_init(res, MLZ_NULL) != 0) {
		mlz_free(res);
		return MLZ_NULL;
	}
	return (mlz_mutex)res;
}

mlz_bool mlz_mutex_destroy(mlz_mutex mutex)
{
	MLZ_RET_FALSE(mutex && pthread_mutex_destroy((pthread_mutex_t *)mutex) == 0);
	mlz_free(mutex);
	return MLZ_TRUE;
}

mlz_bool mlz_mutex_lock(mlz_mutex mutex)
{
	return mutex && pthread_mutex_lock((pthread_mutex_t *)mutex) == 0;
}

mlz_bool mlz_mutex_unlock(mlz_mutex mutex)
{
	return mutex && pthread_mutex_unlock((pthread_mutex_t *)mutex) == 0;
}

typedef struct
{
	pthread_cond_t    cond;
	pthread_mutex_t   mutex;
	volatile mlz_bool flag;
} mlz_event_wrapper;

mlz_event mlz_event_create()
{
	mlz_event_wrapper *res = (mlz_event_wrapper *)mlz_malloc(sizeof(mlz_event_wrapper));
	MLZ_RET_FALSE(res);

	res->flag = MLZ_FALSE;

	if (pthread_cond_init(&res->cond, MLZ_NULL) != 0) {
		mlz_free(res);
		return MLZ_NULL;
	}

	if (pthread_mutex_init(&res->mutex, MLZ_NULL) != 0) {
		(void)pthread_cond_destroy(&res->cond);
		mlz_free(res);
		return MLZ_NULL;
	}

	return (mlz_event)res;
}

mlz_bool mlz_event_destroy(mlz_event event)
{
	mlz_event_wrapper *ew;
	MLZ_RET_FALSE(event);

	ew = (mlz_event_wrapper *)event;

	if (pthread_cond_destroy(&ew->cond) != 0)
		return MLZ_FALSE;
	if (pthread_mutex_destroy(&ew->mutex) != 0)
		return MLZ_FALSE;

	mlz_free(event);
	return MLZ_TRUE;
}

mlz_bool mlz_event_reset(mlz_event event)
{
	mlz_event_wrapper *ew;
	MLZ_RET_FALSE(event);

	ew = (mlz_event_wrapper *)event;

	MLZ_RET_FALSE(pthread_mutex_lock(&ew->mutex) == 0);
	ew->flag = MLZ_FALSE;
	return pthread_mutex_unlock(&ew->mutex) == 0;
}

mlz_bool mlz_event_set(mlz_event event)
{
	mlz_bool res;
	mlz_event_wrapper *ew;
	MLZ_RET_FALSE(event);

	ew = (mlz_event_wrapper *)event;

	MLZ_RET_FALSE(pthread_mutex_lock(&ew->mutex) == 0);
	ew->flag = MLZ_TRUE;
	res =  pthread_cond_broadcast(&ew->cond) == 0;
	return pthread_mutex_unlock(&ew->mutex)  == 0 && res;
}

mlz_bool mlz_event_wait(mlz_event event)
{
	mlz_event_wrapper *ew;
	mlz_bool           res = MLZ_TRUE;
	MLZ_RET_FALSE(event);

	ew = (mlz_event_wrapper *)event;

	MLZ_RET_FALSE(pthread_mutex_lock(&ew->mutex) == 0);
	while (!ew->flag)
		if (pthread_cond_wait(&ew->cond, &ew->mutex) != 0) {
			res = MLZ_FALSE;
			break;
		}

	return pthread_mutex_unlock(&ew->mutex) == 0 && res;
}

typedef struct
{
	pthread_t thread;
	mlz_bool  stopped;
} mlz_thread_wrapper;

mlz_thread mlz_thread_create(void)
{
	mlz_thread_wrapper *handle;
	mlz_thread          res = (mlz_thread)mlz_malloc(sizeof(struct mlz_thread));
	MLZ_RET_FALSE(res);

	handle = (mlz_thread_wrapper *)mlz_malloc(sizeof(mlz_thread_wrapper));
	if (!handle) {
		mlz_free(res);
		return MLZ_NULL;
	}

	res->handle     = handle;
	res->proc       = MLZ_NULL;
	res->param      = MLZ_NULL;
	handle->stopped = MLZ_TRUE;

	return res;
}

static void *mlz_posix_thread_proc(void *param)
{
	mlz_thread thread = (mlz_thread)param;
	thread->proc(thread->param);
	return MLZ_NULL;
}

mlz_bool mlz_thread_run(
	mlz_thread      thread,
	mlz_thread_proc proc,
	void           *param
)
{
	mlz_thread_wrapper *tw;
	MLZ_RET_FALSE(thread && proc);
	thread->proc  = proc;
	thread->param = param;
	tw = (mlz_thread_wrapper *)thread->handle;

	MLZ_RET_FALSE(tw->stopped &&
		pthread_create(&tw->thread, MLZ_NULL, mlz_posix_thread_proc, thread) == 0);

	tw->stopped = MLZ_FALSE;

	return MLZ_TRUE;
}

mlz_bool mlz_thread_destroy(mlz_thread thread)
{
	MLZ_RET_FALSE(thread && mlz_thread_join(thread));

	if (thread->handle)
		mlz_free(thread->handle);

	mlz_free(thread);
	return MLZ_TRUE;
}

mlz_bool mlz_thread_join(mlz_thread thread)
{
	void               *ret_val;
	mlz_bool            res;
	mlz_thread_wrapper *tw;

	MLZ_RET_FALSE(thread);
	tw = (mlz_thread_wrapper *)thread->handle;

	res = tw->stopped || pthread_join(tw->thread, &ret_val) == 0;
	if (res)
		tw->stopped = MLZ_TRUE;

	return res;
}

#endif

static void mlz_job_worker_proc(void *param)
{
	mlz_job_thread *jt = (mlz_job_thread *)param;
	mlz_jobs jobs      = jt->jobs;

	for (;;) {
		mlz_bool done;

		(void)mlz_event_wait(jt->event);
		(void)mlz_event_reset(jt->event);
		if (jt->stop)
			break;
		MLZ_ASSERT(jt->job.job);
		jt->job.job(jt->job.idx, jt->job.param);

		MLZ_ASSERT(jobs->running);

		(void)mlz_mutex_lock(jobs->mutex);
		MLZ_ASSERT(jt->active);
		jt->active = MLZ_FALSE;
		done = !--jobs->active_threads;
		(void)mlz_mutex_unlock(jobs->mutex);

		if (done)
			(void)mlz_event_set(jobs->queue_done_event);
	}
}

mlz_jobs mlz_jobs_create(int num_threads)
{
	mlz_jobs res;
	int      i;
	size_t   size = sizeof(struct mlz_jobs) + sizeof(mlz_job_thread)*(num_threads-1);

	MLZ_RET_FALSE(num_threads > 0);
	res = (mlz_jobs)mlz_malloc(size);
	MLZ_RET_FALSE(res);

	memset(res, 0, size);

	res->mutex            = mlz_mutex_create();
	res->queue_done_event = mlz_event_create();

	if (!res->mutex || !res->queue_done_event || !mlz_event_reset(res->queue_done_event)) {
		(void)mlz_jobs_destroy(res);
		return MLZ_NULL;
	}

	for (i=0; i<num_threads; i++) {
		mlz_job_thread *jt = res->thread + i;
		jt->jobs = res;
		jt->event = mlz_event_create();
		jt->thread = mlz_thread_create();

		if (!jt->event || !jt->thread || !mlz_thread_run(jt->thread, mlz_job_worker_proc, jt)) {
			(void)mlz_jobs_destroy(res);
			return MLZ_NULL;
		}
	}

	res->num_threads = num_threads;

	return res;
}

mlz_bool mlz_jobs_destroy(mlz_jobs jobs)
{
	int i;

	MLZ_RET_FALSE(mlz_jobs_wait(jobs));
	MLZ_RET_FALSE(jobs);

	for (i=0; i<jobs->num_threads; i++) {
		mlz_job_thread *jt = jobs->thread + i;

		if (jt->thread) {
			jt->stop = MLZ_TRUE;
			MLZ_RET_FALSE(mlz_event_set(jt->event) && mlz_thread_join(jt->thread));
			MLZ_RET_FALSE(mlz_thread_destroy(jt->thread));
			jt->thread = MLZ_NULL;
		}

		MLZ_RET_FALSE(mlz_event_destroy(jt->event));
		jt->event = MLZ_NULL;
	}

	if (jobs->queue_done_event) {
		MLZ_RET_FALSE(mlz_event_destroy(jobs->queue_done_event));
		jobs->queue_done_event = MLZ_NULL;
	}

	if (jobs->mutex) {
		MLZ_RET_FALSE(mlz_mutex_destroy(jobs->mutex));
		jobs->mutex = MLZ_NULL;
	}

	mlz_free(jobs);
	return MLZ_TRUE;
}

mlz_bool mlz_jobs_enqueue(mlz_jobs jobs, mlz_job job)
{
	int i, num;
	mlz_bool set_running = MLZ_FALSE;

	MLZ_RET_FALSE(jobs && job.job);

	if (!jobs->running) {
		MLZ_RET_FALSE(mlz_event_reset(jobs->queue_done_event));
		set_running = jobs->running = MLZ_TRUE;
	}

	MLZ_RET_FALSE(mlz_mutex_lock(jobs->mutex));

	num = jobs->num_threads;
	{
		mlz_bool res;
		mlz_job_thread *jt = jobs->thread;

		while (jt->active)
			++jt;

		MLZ_ASSERT(jt - jobs->thread < num);

		jt->active = MLZ_TRUE;
		jt->job    = job;

		res = mlz_mutex_unlock(jobs->mutex) && mlz_event_set(jt->event);
		if (!res && set_running)
			jobs->running = MLZ_FALSE;

		return res;
	}
	return mlz_mutex_unlock(jobs->mutex);
}

mlz_bool mlz_jobs_prepare_batch(
	mlz_jobs jobs,
	mlz_int  num_threads
)
{
	MLZ_RET_FALSE(jobs && mlz_mutex_lock(jobs->mutex));

	MLZ_ASSERT(num_threads >= 0);
	jobs->active_threads = num_threads;

	return mlz_mutex_unlock(jobs->mutex);
}

mlz_bool mlz_jobs_wait(mlz_jobs jobs)
{
	mlz_bool res;
	MLZ_RET_FALSE(jobs);
	res = !jobs->running || mlz_event_wait(jobs->queue_done_event);
	jobs->running = MLZ_FALSE;
	return res;
}

#endif
