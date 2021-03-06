#define _GNU_SOURCE

#include "easythread.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/sysctl.h>
#include <sched.h>
#include <pthread.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>

#define sizeal(TY) (sizeof(TY) + sizeof(TY) % sizeof(INT32)) 
#define tooffset(M,OF) ((BYTE*)(M) + (OF))

typedef struct __SEMAPHORE
{
	sem_t s;
}_SEMAPHORE;

typedef struct __MUTEX
{
    pthread_mutex_t mutex;
}_MUTEX;

typedef struct __MUTEN
{
	INT32 memid;
	CHAR* membase;
	CHAR* memname;
	pthread_mutex_t* mx;
	pthread_mutexattr_t* att;
}_MUTEN;

typedef struct __BARRIER
{
    pthread_barrier_t barrier;
}_BARRIER;

typedef struct __EVENT
{
    MUTEX mutex;
    pthread_cond_t condition;
    INT32 autoenter;
    INT32 autoexit;
    INT32 value;
    INT32 resumeall;
}_EVENT;

typedef struct __MESSAGE
{
    THRMESSAGE type;
    VOID* msg;
    INT32 autofree;
    struct __MESSAGE* next;
}_MESSAGE;

typedef struct __MSGQUEUE
{
    _MESSAGE* first;
    _MESSAGE* last;
    _MESSAGE* current;

    MUTEX safeins;
    EVENT havemsg;
    INT32 szcoda;
}_MSGQUEUE;

typedef struct _TALKASK
{
	INT32 idreply;
	INT32 sz;
	BYTE  question[THR_TALK_MSG_SZ];
}TALKASK;

typedef struct _TALKREPLY
{
	BOOL inuse;
	pthread_mutex_t lock;
	pthread_mutexattr_t alock;
	INT32 sz;
	BYTE  answer[THR_TALK_MSG_SZ];
}TALKREPLY;

typedef struct __TALKQUEUE
{
	INT32 memid;
	INT32 memsz;
	VOID* memadr;
		
	pthread_mutex_t* lock;
	pthread_mutexattr_t* alock;
	pthread_mutex_t* submit;
	pthread_mutexattr_t* asubmit;
	
	INT32* askin;
	INT32* askou;
	INT32  askco;
	TALKASK* ask;
	
	TALKREPLY* reply;
	INT32 replyco;
}_TALKQUEUE;


typedef struct __WORKER
{
    INT32 autofree;
    VOID* param;

    INT32 priority;
    INT32 priostat;

    FLOAT64 timer;
    FLOAT64 elapse;

    WORKCALL dowork;
    WORKCALL onprogress;
    WORKCALL oncomplete;
    WORKCALL tofree;

    struct __WORKER* next;
    struct __WORKER* prev;
}_WORKER;

typedef struct __WORK
{
    _WORKER* first;
    _WORKER* last;
    _WORKER* current;

    MUTEX safeins;
    EVENT havework;
    INT32 timemode;
}_WORK;

typedef struct __THR
{
    pthread_t id;
    pthread_attr_t att;
    THRCALL fnc;
    INT32 runsuspend;
    EVENT suspend;
    EVENT finish;
    VOID* param;
    THRMODE stato;
}_THR;

typedef struct __JOB
{
    THR* j;
    INT32 n;
}_JOB;

/// /////// ///
/// SUPPORT ///
/// /////// ///

static FLOAT64 _bch_get()
{
    struct timeval t;
    struct timezone tzp;
    gettimeofday(&t, &tzp);
    return t.tv_sec + t.tv_usec*1e-6;
}

static inline FLOAT64 _bch_clc(double st,double en)
{
    return en-st;
}

static cpu_set_t* _setcpu(UINT32 mcpu)
{
	static cpu_set_t cpu;
	CPU_ZERO(&cpu);
	
	if (mcpu == 0 ) return &cpu;
	
	UINT32 s;
	while ( (s = mcpu % 10) )
	{
		CPU_SET(s - 1,&cpu);
		mcpu /= 10;
	}
	
	return &cpu;
}

/// ///////// ///
/// SEMAPHORE ///
/// ///////// ///

SEMAPHORE thr_semaphore_new(UINT32 stval)
{
	_SEMAPHORE* s = malloc(sizeof(_SEMAPHORE));
	if ( sem_init(&s->s,0,stval) ) {free(s); return NULL;}
	return s;
}

inline VOID thr_semaphore_wait(SEMAPHORE s)
{
	sem_wait(&s->s);
}

inline VOID thr_semaphore_post(SEMAPHORE s)
{
	sem_post(&s->s);
}

inline INT32 thr_semaphore_get(SEMAPHORE s)
{
	INT32 ret;
	sem_getvalue(&s->s,&ret);
	return ret;
}

VOID thr_semaphore_free(SEMAPHORE s)
{
	sem_destroy(&s->s);
	free(s);
}

/// ///// ///
/// MUTEX ///
/// ///// ///

MUTEX thr_mutex_new()
{
    _MUTEX* m= malloc (sizeof(_MUTEX));
    pthread_mutex_init(&m->mutex,NULL);
    return m;
}

inline VOID thr_mutex_lock(MUTEX m)
{
    pthread_mutex_lock(&m->mutex);
}

inline VOID thr_mutex_unlock(MUTEX m)
{
    pthread_mutex_unlock(&m->mutex);
}

VOID thr_mutex_free(MUTEX m)
{
    pthread_mutex_destroy(&m->mutex);
    free(m);
}

/// ///// ///
/// MUTEN ///
/// ///// ///

MUTEN thr_muten_new(CHAR* filenamespace, CHAR* name)
{	
	
	///printf("Page size: 4096\nmx+att = %d\n",sizeal(pthread_mutex_t) + sizeal(pthread_mutexattr_t));
	///printf("max muten = 4096 / mx+att+name = %d\n",4096 / (sizeal(pthread_mutex_t) + sizeal(pthread_mutexattr_t) + THR_MUTEN_NAME_MAX));
	
	FILE* tst = fopen(filenamespace,"r");
	if ( !tst )
	{
		tst = fopen(filenamespace,"w");
			if ( !tst ) return NULL;
		fprintf(tst,"named mutex %s\n%d",filenamespace,THR_MUTEN_MAX);
		fclose(tst);
	}
	
	key_t k = ftok(filenamespace,'M');
		if ( k < 0 ) {return NULL;}
		
	_MUTEN* h = malloc(sizeof(_MUTEN));
	
	UINT32 sz = sizeal(pthread_mutex_t) + sizeal(pthread_mutexattr_t);
	UINT32 ofch = sz;
	sz += THR_MUTEN_MAX * THR_MUTEN_NAME_MAX;
	UINT32 ofmu = sz;
	sz += THR_MUTEN_MAX * ( sizeal(pthread_mutex_t) + sizeal(pthread_mutexattr_t) );
	
	BOOL nw = FALSE;
	if ( (h->memid = shmget(k,sz,IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR)) < 0 )
	{
		if ( (h->memid = shmget(k,sz,S_IRUSR | S_IWUSR)) < 0 ) {free(h); return NULL;} 
	}
	else
		nw = TRUE;
	
	h->membase = shmat(h->memid, (void *)0, 0);
		if (h->membase == (char *)(-1)) {free(h); return NULL;}
		
	pthread_mutex_t* lkm = (pthread_mutex_t*) h->membase;
	pthread_mutexattr_t* alkm = (pthread_mutexattr_t*) tooffset(h->membase,sizeal(pthread_mutex_t));
	
	CHAR* pn;
	INT32 i;
	
	if ( nw )
	{
		pthread_mutexattr_init(alkm);
		pthread_mutexattr_setpshared(alkm, PTHREAD_PROCESS_SHARED);
		pthread_mutex_init(lkm,alkm);
		
		pn = (CHAR*) tooffset(h->membase,ofch);
		for (i = 0; i < THR_MUTEN_MAX; ++i, pn += THR_MUTEN_NAME_MAX)
			*pn = '\0';
	}
	pthread_mutex_lock(lkm);
	
	for (pn = (CHAR*) tooffset(h->membase,ofch), i = 0; i < THR_MUTEN_MAX && strcmp(pn,name); ++i, pn += THR_MUTEN_NAME_MAX);
	
	if ( i >= THR_MUTEN_MAX )
	{
		for (pn = (CHAR*) tooffset(h->membase,ofch), i = 0; i < THR_MUTEN_MAX && *pn; ++i, pn += THR_MUTEN_NAME_MAX);
		if ( i >= THR_MUTEN_MAX ) { free(h); return NULL;}
		
		strcpy(pn,name);
		h->memname = pn;
		h->mx =(pthread_mutex_t*) tooffset(h->membase,ofmu + ((sizeal(pthread_mutex_t) + sizeal(pthread_attr_t)) * i));
		h->att = (pthread_mutexattr_t*) tooffset(h->mx,sizeal(pthread_mutex_t));
		pthread_mutexattr_init(h->att);
		pthread_mutexattr_setpshared(h->att, PTHREAD_PROCESS_SHARED);
		pthread_mutex_init(h->mx,h->att);
	}
	else
	{
		h->mx =(pthread_mutex_t*) tooffset(h->membase,ofmu + ((sizeal(pthread_mutex_t) + sizeal(pthread_attr_t)) * i));
		h->att = (pthread_mutexattr_t*) tooffset(h->mx,sizeal(pthread_mutex_t));
	}
	pthread_mutex_unlock(lkm);
	
	return h;
}

inline VOID thr_muten_lock(MUTEN m)
{
    pthread_mutex_lock(m->mx);
}

inline VOID thr_muten_unlock(MUTEN m)
{
    pthread_mutex_unlock(m->mx);
}

VOID thr_muten_destroy(CHAR* filenamespace, MUTEN m)
{
	INT32 memid = m->memid;
	thr_muten_free(m);
	shmctl(memid,IPC_RMID,0);
	unlink(filenamespace);
}

VOID thr_muten_free(MUTEN m)
{
	pthread_mutex_destroy(m->mx);
	pthread_mutexattr_destroy(m->att);
	m->memname = '\0';
	shmdt(m->membase);
	free(m);
}

/// /////// ///
/// BARRIER ///
/// /////// ///

BARRIER thr_barrier_new(int nthread)
{
    _BARRIER* b = malloc (sizeof(_BARRIER));
    pthread_barrier_init(&b->barrier,NULL,nthread);
    return b;
}

inline VOID thr_barrier_enter(BARRIER b)
{
    pthread_barrier_wait(&b->barrier);
}

VOID thr_barrier_free(BARRIER b)
{
    pthread_barrier_destroy(&b->barrier);
    free(b);
}

/// ////// ///
/// EVENTO ///
/// ////// ///

EVENT thr_event_new(INT32 autoenter,INT32 autoexit,INT32 resumeall,INT32 value)
{
    _EVENT* e= malloc (sizeof(_EVENT));
    e->autoenter = autoenter;
    e->autoexit = autoexit;
    e->resumeall = resumeall;
    e->value = 0;
    e->mutex = thr_mutex_new();
    pthread_cond_init(&e->condition, NULL);
    return e;
}

VOID thr_event_free(EVENT e)
{
    thr_mutex_free(e->mutex);
    pthread_cond_destroy(&e->condition);
    free(e);
}

VOID thr_event_enter(EVENT e)
{
    if (e->autoenter) return;
    thr_mutex_lock(e->mutex);
}

VOID thr_event_exit(EVENT e)
{
    if (e->autoexit) return;
    thr_mutex_unlock(e->mutex);
}

INT32 thr_event_wait(EVENT e,INT32 timeoutms)
{
    if (e->autoenter)
        thr_mutex_lock(e->mutex);

    if (!e->value)
    {
        if (!timeoutms)
        {
            pthread_cond_wait(&e->condition, &e->mutex->mutex);
        }
        else
        {
            struct timespec   ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += timeoutms * 1000;


            INT32 ret = pthread_cond_timedwait(&e->condition, &e->mutex->mutex, &ts);
            if (ret)
            {
                ret = 0;
                thr_mutex_unlock(e->mutex);
                return 0;
            }
        }

    }

    if (e->autoexit)
    {
        thr_mutex_unlock(e->mutex);
    }

    return 1;
}

VOID thr_event_raise(EVENT e)
{
    thr_mutex_lock(e->mutex);
        e->value = 1;


    if (e->resumeall)
        pthread_cond_broadcast(&e->condition);
    else
    {
        pthread_cond_signal(&e->condition);
    }

    thr_mutex_unlock(e->mutex);
}

VOID thr_event_reset(EVENT e)
{
    thr_mutex_lock(e->mutex);
    e->value = 0;
    thr_mutex_unlock(e->mutex);
}

/// /////// ///
/// MESSAGE ///
/// /////// ///

MESSAGE thr_message_new(THRMESSAGE type,VOID* data,INT32 autofree)
{
    _MESSAGE* m = (_MESSAGE*)malloc(sizeof(_MESSAGE));
    m->type = type;
    m->msg = data;
    m->autofree = autofree;
    m->next = NULL;
    return m;
}

VOID thr_message_free(MESSAGE m)
{
    if ( m == NULL ) return;
    if (m->autofree && m->msg != NULL) free(m->msg);
    free(m);
}

THRMESSAGE thr_message_gettype(MESSAGE m) { return m->type; }
VOID* thr_message_getmsg(MESSAGE m) { return m->msg; }
INT32 thr_message_getautofree(MESSAGE m) { return m->autofree; }

/// //////// ///
/// MSGQUEUE ///
/// //////// ///

MSGQUEUE thr_queue_new()
{
    _MSGQUEUE* q = (_MSGQUEUE*)malloc(sizeof(_MSGQUEUE));

    q->szcoda = 0;
    q->first = NULL;
    q->last = NULL;
    q->current = NULL;


    q->safeins = thr_mutex_new();
    q->havemsg = thr_event_new(1,1,0,0);

    return q;
}

INT32 thr_queue_free(MSGQUEUE q)
{
    if (q == NULL) return 0;

    for ( ; q->first != NULL ; q->first = q->current)
    {
        q->current = q->first->next;
        thr_message_free(q->first);
    }

    thr_mutex_free(q->safeins);
    thr_event_free(q->havemsg);
    free(q);

    return 0;
}

inline INT32 thr_queue_getsize(MSGQUEUE q) {return q->szcoda;}

INT32 thr_queue_add(MSGQUEUE q,MESSAGE m)
{
    thr_mutex_lock(q->safeins);

    m->next = NULL;

    if (q->first == NULL)
    {
        q->first = m;
        q->last = m;
    }
    else
    {
        q->last->next = m;
        q->last = m;
    }


    ++q->szcoda;

    thr_mutex_unlock(q->safeins);
    thr_event_raise(q->havemsg);
    return 0;
}

MESSAGE thr_queue_getmessage(MSGQUEUE q, UINT32 waitms)
{
    if (q == NULL) return NULL;

    if (waitms && q->szcoda <= 0)
    {
        thr_event_wait(q->havemsg,waitms);
        thr_event_reset(q->havemsg);
    }

    thr_mutex_lock(q->safeins);

    _MESSAGE* r;

    if (q->first == NULL)
    {
        r = NULL;
    }
    else
    {
        r = q->first;
        if (q->first == q->last)
        {
            q->first = NULL;
            q->last = NULL;
        }
        else
        {
            q->first = q->first->next;
        }
        --q->szcoda;
    }
    thr_mutex_unlock(q->safeins);

    return r;
}

/// ///////// ///
/// TALKQUEUE ///
/// ///////// ///

TALKQUEUE thr_talk_new(CHAR* tpath, INT32 maxask, INT32 maxreply)
{
	_TALKQUEUE* t = malloc( sizeof( _TALKQUEUE ) );
	
	t->askco = maxask;
	t->replyco = maxreply;
	
	INT32 asksz = maxask * sizeal(TALKASK);
	INT32 replysz = maxreply * sizeal(TALKREPLY);
	
	t->memsz = sizeal(pthread_mutex_t) * 2 + sizeal(pthread_attr_t) * 2;
	t->memsz += asksz + replysz + sizeof(INT32) * 2;
	
	if ( !tpath )
	{
		t->memid = 0;
		t->memadr = malloc(t->memsz);
			if ( !t->memadr ) return NULL;
	}
	else
	{
		FILE* ft = fopen(tpath,"r");
			if ( ft ) { fclose(ft); free(t); return NULL;}
	
		ft = fopen(tpath,"w");
			if ( !ft ) { free(t); return NULL;}
		fprintf(ft,"TalkQueue %s\n%d\n%d\n%d",tpath,t->askco,t->replyco,t->memsz);
		fclose(ft);
	
		key_t k = ftok(tpath,'T');
			if ( k < 0 ) { free(t); return NULL;}
	
		if ( (t->memid = shmget(k,t->memsz,IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR)) < 0 )
		{
			free(t);
			unlink(tpath);
			return NULL;
		}
		
		t->memadr = shmat(t->memid, (void *)0, 0);
			if (t->memadr == (char *)(-1)) {free(t); unlink(tpath); return NULL;}
	}
	
	t->lock = (pthread_mutex_t*) t->memadr;
	t->alock = (pthread_mutexattr_t*) tooffset(t->lock, sizeal(pthread_mutex_t));
	t->submit = (pthread_mutex_t*) tooffset(t->alock, sizeal(pthread_mutexattr_t));
	t->asubmit = (pthread_mutexattr_t*) tooffset(t->submit, sizeal(pthread_mutex_t));
	
	pthread_mutexattr_init(t->alock);
	pthread_mutexattr_setpshared(t->alock, PTHREAD_PROCESS_SHARED);
	pthread_mutex_init(t->lock,t->alock);
	
	pthread_mutexattr_init(t->asubmit);
	pthread_mutexattr_setpshared(t->asubmit, PTHREAD_PROCESS_SHARED);
	pthread_mutex_init(t->submit,t->asubmit);
	///init
	pthread_mutex_lock(t->lock);
		///alloc shm
		t->askin = (INT32*) tooffset(t->asubmit, sizeal(pthread_mutexattr_t));
		t->askou = (INT32*) tooffset(t->askin, sizeof(INT32));
		t->ask   = (TALKASK*) tooffset(t->askou, sizeof(INT32));
		t->reply = (TALKREPLY*) tooffset(t->ask, asksz);
		
		///reset;
		INT32 i;
		for ( i = 0; i < t->askco; ++i )
		{
			t->ask[i].idreply = -1;
			t->ask[i].sz = 0;
		}
		*t->askin = *t->askou = 0;
		
		for ( i = 0; i < t->replyco; ++i )
		{
			t->reply[i].inuse = FALSE;
			pthread_mutexattr_init(&t->reply[i].alock);
			pthread_mutexattr_setpshared(&t->reply[i].alock, PTHREAD_PROCESS_SHARED);
			pthread_mutex_init(&t->reply[i].lock,&t->reply[i].alock);
			t->reply[i].sz = 0;
		}
		
		pthread_mutex_lock(t->submit);
	pthread_mutex_unlock(t->lock);
	
	return t;
}  

VOID thr_talk_free(TALKQUEUE t, CHAR* tpath)
{
	pthread_mutex_lock(t->lock);
	pthread_mutex_unlock(t->lock);
	
	if (tpath) unlink(tpath);
	
	INT32 i;
	for ( i = 0; i < t->replyco; ++i )
	{
		pthread_mutexattr_destroy(&t->reply[i].alock); 
		pthread_mutex_destroy(&t->reply[i].lock);
	}
		
	pthread_mutexattr_destroy(t->alock); 
	pthread_mutex_destroy(t->lock);
	pthread_mutexattr_destroy(t->asubmit); 
	pthread_mutex_destroy(t->submit);
	
	if ( tpath )
	{
		shmdt(t->memadr);
		shmctl(t->memid,IPC_RMID,0);
	}
	else
	{
		free(t->memadr);
	}
	
	free(t);
}  

TALKQUEUE thr_talk_hook(CHAR* tpath)
{
	if ( !tpath ) return NULL;
	
	_TALKQUEUE* t = malloc( sizeof( _TALKQUEUE ) );
	
	FILE* ft = fopen(tpath,"r");
		if ( !ft ) { free(t); return NULL;}
		CHAR buf[256];
		fgets(buf,256,ft);
		fgets(buf,256,ft);
		t->askco = atoi(buf);
		fgets(buf,256,ft);
		t->replyco = atoi(buf);
		fgets(buf,256,ft);
		t->memsz = atoi(buf);
	fclose(ft);
	
	key_t k = ftok(tpath,'T');
		if ( k < 0 ) { free(t); return NULL;}
	
	if ( (t->memid = shmget(k,t->memsz,S_IRUSR | S_IWUSR)) < 0 ) {free(t); return NULL;} 
		
	t->memadr = shmat(t->memid, (void *)0, 0);
		if (t->memadr == (char *)(-1)) {free(t); return NULL;}
	
	t->lock = (pthread_mutex_t*) t->memadr;
	t->alock = (pthread_mutexattr_t*) tooffset(t->lock, sizeal(pthread_mutex_t));
	t->submit = (pthread_mutex_t*) tooffset(t->alock, sizeal(pthread_mutexattr_t));
	t->asubmit = (pthread_mutexattr_t*) tooffset(t->submit, sizeal(pthread_mutex_t));
	
	t->askin = (INT32*) tooffset(t->asubmit, sizeal(pthread_mutexattr_t));
	t->askou = (INT32*) tooffset(t->askin, sizeof(INT32));
	t->ask   = (TALKASK*) tooffset(t->askou, sizeof(INT32));
	t->reply = (TALKREPLY*) tooffset(t->ask, t->askco * sizeal(TALKASK) );
	
	return t;
}  

VOID thr_talk_unhook(TALKQUEUE t)
{
	shmdt(t->memadr);
}

BOOL _talk_askfull(_TALKQUEUE* t)
{
	if ( *t->askin + 1 == *t->askou ) return TRUE;
	if ( *t->askin + 1 >= t->askco && !*t->askou ) return TRUE;
	return FALSE;
}

INT32 _talk_requestreply(_TALKQUEUE* t)
{
	INT32 i;
	for ( i = 0; i < t->replyco; ++i )
		if ( !t->reply[i].inuse ) return i;
	return -1;
}

INT32 thr_talk_ask(TALKQUEUE t, VOID* question, INT32 sz, BOOL wantanswer, BOOL forcequestion)
{
	INT32 idr = -1;
	
	pthread_mutex_lock(t->lock);
		do
		{
			if ( _talk_askfull(t) || ( wantanswer && (idr = _talk_requestreply(t)) == -1 ) )
			{ 
				pthread_mutex_unlock(t->lock);
					thr_msleep(1);
				pthread_mutex_lock(t->lock);	
			}
			else
			{
				break;
			}
		}while(forcequestion);
		 
		t->ask[*t->askin].idreply = idr;
		t->ask[*t->askin].sz = sz;
		memcpy(t->ask[*t->askin].question,question,sz);
		
		if (wantanswer)
		{
			t->reply[idr].inuse = TRUE;
			pthread_mutex_lock(&t->reply[idr].lock);
			t->reply[idr].sz = 0;
		}
		
		++(*t->askin);
		if ( *t->askin >= t->askco ) *t->askin = 0;
		
	pthread_mutex_unlock(t->lock);
	pthread_mutex_unlock(t->submit);
	
	return idr;
}

INT32 thr_talk_reply(TALKQUEUE t, INT32 idr, VOID* answer, INT32 sz)
{
	pthread_mutex_lock(t->lock);
		memcpy(t->reply[idr].answer,answer,sz);
		t->reply[idr].sz = sz;
	pthread_mutex_unlock(t->lock);
	
	pthread_mutex_unlock(&t->reply[idr].lock);
	
	return 0;
}

INT32 thr_talk_waitask(TALKQUEUE t, VOID* question, INT32* sz)
{
	pthread_mutex_lock(t->submit);

	INT32 idr;
	
	pthread_mutex_lock(t->lock);
		idr = t->ask[*t->askou].idreply;
		*sz = t->ask[*t->askou].sz;
		memcpy(question,t->ask[*t->askou].question,*sz);
		
		++(*t->askou);
		if ( *t->askou >= t->askco ) *t->askou = 0;
	pthread_mutex_unlock(t->lock);
	
	return idr;
}

INT32 thr_talk_waitanswer(TALKQUEUE t, INT32 idr, VOID* answer, INT32* sz)
{
	pthread_mutex_lock(&t->reply[idr].lock);
	pthread_mutex_unlock(&t->reply[idr].lock);
	
	*sz = t->reply[idr].sz;
	memcpy(answer,t->reply[idr].answer,*sz);
	
	pthread_mutex_lock(t->lock);
		t->reply[idr].inuse = FALSE;
	pthread_mutex_unlock(t->lock);
	
	return 0;
}


VOID thr_talk_arforsize(INT32* nask, INT32* nreply, UINT32 sz)
{	
	UINT32 ps = 4096 * (sz / 4096); //sysconf(_SC_PAGESIZE) * (sz / sysconf(_SC_PAGESIZE));
	
	ps -= sizeal(pthread_mutex_t) * 2 + sizeal(pthread_attr_t) * 2 + sizeof(INT32) * 2  ;
	ps >>= 1;
	
	*nask = ps / sizeal(TALKASK);
	*nreply = ps / sizeal(TALKREPLY);
}


/// ////// ///
/// WORKER ///
/// ////// ///

WORKER thr_worker_new(WORKCALL dowork,WORKCALL progress,WORKCALL complete,WORKCALL tofree,INT32 autofree,VOID* param,INT32 priority)
{
    _WORKER* w = (_WORKER*)malloc(sizeof(_WORKER));

    w->dowork = dowork;
    w->onprogress = progress;
    w->oncomplete = complete;
    w->tofree = tofree;

    w->autofree = autofree;
    w->param = param;

    w->priority = priority;
    w->priostat = 0;

    w->timer = 0.0;
    w->elapse = 0.0;

    w->next = NULL;
    w->prev = NULL;

    return w;
}

/// //// ///
/// WORK ///
/// //// ///

WORK thr_work_new()
{
    _WORK* w = (_WORK*)malloc(sizeof(_WORK));

    w->first = NULL;
    w->last = NULL;
    w->current = NULL;
    w->timemode = 0;

    w->safeins = thr_mutex_new();
    w->havework = thr_event_new(1,1,0,0);

    return w;
}

INT32 thr_work_free(WORK w)
{
    if (w == NULL) return 0;
    thr_mutex_lock(w->safeins);

    for ( ; w->first != NULL ; w->first = w->current)
    {
        w->current = w->first->next;
        if (w->first->autofree)
            {if (w->first->param) free(w->first->param);}
        else if (w->first->tofree)
            w->first->tofree(NULL,w->first->param);

        free(w->first);
    }

    thr_mutex_unlock(w->safeins);
    thr_mutex_free(w->safeins);
    thr_event_free(w->havework);
    free(w);

    return 0;

}

INT32 thr_work_add(WORK w,WORKER wr)
{
    thr_mutex_lock(w->safeins);

    wr->next = NULL;
    wr->prev = NULL;
    if (wr->timer == 0.0) ++w->timemode;

    if (w->first == NULL)
    {
        w->first = wr;
        w->last = wr;
        wr->priostat = wr->priority;
    }
    else
    {
        --wr->priostat;
        if (wr->priostat > 0)
        {
            _WORKER* i;

            for (i = w->first; i  != NULL && wr->priority <= i->priority; i = i->next);

            if (i == NULL)
            {
                wr->prev = w->last;
                w->last->next = wr;
                w->last = wr;
            }
            else
            {
                if (i->prev != NULL)
                    i->prev->next = wr;
                else
                    w->first = wr;

                wr->prev = i->prev;
                i->prev = wr;
                wr->next = i;
            }
        }
        else
        {
            wr->prev = w->last;
            w->last->next = wr;
            w->last = wr;
            wr->priostat = wr->priority;
        }
    }

    thr_event_raise(w->havework);
    thr_mutex_unlock(w->safeins);


    return 0;
}

INT32 thr_work_run(WORK w)
{
    if (w == NULL) return -1;

    w->current = NULL;
    INT32 retw;
    INT32 mss = 0;

    while (1)
    {
        thr_event_wait(w->havework,mss);

        thr_mutex_lock(w->safeins);
            if (w->first == NULL)
            {
                mss = 0;
                thr_event_reset(w->havework);
                thr_mutex_unlock(w->safeins);
                continue;
            }

            if (!w->timemode)
            {
                FLOAT64 minsleep=99999.0;
                FLOAT64 ca;
                _WORKER* i;

                for (i = w->first ; i != NULL ; i = i->next)
                {
                    ca = i->timer - _bch_clc(i->elapse,_bch_get());
                    if ( ca < minsleep)
                    {
                        minsleep = ca;
                    }
                }

                if (ca >= 0.0)
                {
                    mss = ca * 1001.0;
                    if (mss < 1) mss = 1;
                    thr_event_reset(w->havework);
                }
            }
            else
            {
                mss = 0;
            }

            w->current = w->first;
            w->first = w->first->next;
            if (w->first != NULL)
                w->first->prev = NULL;
            else
            {
                mss = 0;
                thr_event_reset(w->havework);
            }
            if (w->current->timer == 0.0) --w->timemode;


        thr_mutex_unlock(w->safeins);

        if (w->current->priority == THR_WORK_PRIORITY_END) break;

        if ( w->current->timer > 0.0 )
        {
            if ( _bch_clc(w->current->elapse,_bch_get()) >= w->current->timer)
            {
                w->current->timer = 0.0;
                if (w->current->dowork != NULL)
                    retw = w->current->dowork(w,w->current->param);
                else
                    retw = THR_WORK_COMPLETE;
            }
            else
            {
                retw = THR_WORK_SKIP;
            }
        }
        else
        {
            if (w->current->dowork != NULL)
                retw = w->current->dowork(w,w->current->param);
            else
                retw = THR_WORK_COMPLETE;
        }

        if (retw > 0)
        {
            w->current->elapse = _bch_get();
            w->current->timer = (double)(retw) / 1000.0;
            if (w->current->onprogress != NULL)
                w->current->onprogress(w,w->current->param);

            thr_work_add(w,w->current);
        }
        else if (retw == THR_WORK_COMPLETE)
        {
            if (w->current->oncomplete != NULL)
                retw = w->current->oncomplete(w,w->current->param);

            if (w->current->autofree)
                {if (w->current->param) free(w->current->param);}
            else if (w->current->tofree != NULL)
                w->current->tofree(NULL,w->current->param);
            free(w->current);
            w->current = NULL;
            if (retw == THR_WORK_COMPLETE_EXIT)
                break;
        }
        else if (retw == THR_WORK_CONTINUE)
        {
            if (w->current->onprogress != NULL)
                w->current->onprogress(w,w->current->param);

            thr_work_add(w,w->current);
        }
        else if (retw == THR_WORK_SKIP)
        {
            thr_work_add(w,w->current);
        }
        else
        {
            if (w->current->autofree)
                {if (w->current->param) free(w->current->param);}
            else if (w->current->tofree != NULL)
            {
                w->current->tofree(NULL,w->current->param);
            }
            free(w->current);
            break;
        }

    }

    return 0;
}


/// ////// ///
/// THREAD ///
/// ////// ///

THR thr_new(THRCALL thrcall, UINT32 stksz, INT32 runsuspend, UINT32 oncpu)
{
    _THR* thr = (_THR*) malloc(sizeof(_THR));

    thr->stato = T_CREATE;
    thr->fnc = thrcall;

    pthread_attr_init(&thr->att);
    if ( stksz > 0 ) pthread_attr_setstacksize (&thr->att, stksz);
	
	if ( oncpu > 0 )
	{
		cpu_set_t* ncpu = _setcpu(oncpu);
		pthread_attr_setaffinity_np(&thr->att,CPU_SETSIZE,ncpu);
	}
	
    thr->runsuspend = runsuspend;
    thr->suspend = thr_event_new(1,1,0,0);
    thr->finish = thr_event_new(1,1,0,0);

    return thr;
}

INT32 thr_run(THR t,VOID* param)
{

    if (t->stato > T_CREATE && t->stato < T_END) return 0;

    t->stato = T_RUN;
    t->param = param;
    thr_event_reset(t->suspend);
    thr_event_reset(t->finish);
    pthread_create(&t->id,&t->att,t->fnc,(void*)t);
    return 1;
}

INT32 thr_free(THR t)
{
    if (t->stato != T_END) return 0;

    thr_event_free(t->suspend);
    thr_event_free(t->finish);
    pthread_attr_destroy(&t->att);
    free(t);
    return 1;
}

VOID thr_changecpu(THR t, UINT32 oncpu)
{
	if ( oncpu > 0 )
	{
		cpu_set_t* ncpu = _setcpu(oncpu);
		pthread_attr_setaffinity_np(&t->att,CPU_SETSIZE,ncpu);
	}
}

inline VOID* thr_getparam(THR t)
{
    return t->param;
}

VOID* thr_waitthr(THR t)
{
	if ( !t ) return NULL;
    void* ret;
    pthread_join(t->id,&ret);
    return ret;
}

VOID thr_requestwait(THR t)
{
    if (t->stato != T_RUN) return;
    t->stato = T_PAUSE;
}

VOID thr_resume(THR t)
{
    if (t->stato != T_PAUSE) return;
    thr_event_raise(t->suspend);
}

VOID thr_startsuspend(THR t)
{
    if (!t->runsuspend) return;
    thr_suspendme(t);
}

VOID thr_suspendme(THR t)
{
    t->stato = T_PAUSE;

    thr_event_wait(t->suspend,0);
    thr_event_reset(t->suspend);
    t->stato = T_RUN;

}

VOID thr_chkpause(THR t)
{
    if (t->stato != T_PAUSE) return;
    thr_suspendme(t);
}

INT32 thr_chkrequestend(THR t)
{
    if (t->stato != T_REQUESTEXIT) return 0;
    return 1;
}

INT32 thr_sleep(FLOAT64 sleep_time)
{
	struct timespec tv;

	tv.tv_sec = (time_t) sleep_time;

	tv.tv_nsec = (long) ((sleep_time - tv.tv_sec) * 1e+9);

	while (1)
	{
		int rval = nanosleep (&tv, &tv);
		if (rval == 0)
			return 0;
		else if (errno == EINTR)
			continue;
		else
			return rval;
	}
	return 0;
}

INT32 thr_msleep(UINT32 ms)
{
	FLOAT64 rs = (FLOAT64)(ms) / 1000.0;
    return thr_sleep(rs);
}

INT32 thr_nsleep(UINT32 ns)
{
	FLOAT64 rs = (FLOAT64)(ns) / 1000000.0;
    return thr_sleep(rs);
}


INT32 thr_stop(THR t, UINT32 ms, INT32 forceclose)
{
    if (t->stato == T_END) return 1;
    if (t->stato != T_RUN) return 0;
    THRMODE old = t->stato;
    t->stato = T_REQUESTEXIT;
    int tr = thr_event_wait(t->finish,ms);
    if (!tr)
    {
        if (forceclose)
        {
            pthread_cancel(t->id);
            t->stato = T_END;
        }
        else
        {
            t->stato = old;
            return 0;
        }
    }
    thr_event_reset(t->finish);
    return 1;
}

VOID thr_exit(THR t,VOID* ret)
{
    thr_event_raise(t->finish);
    t->stato=T_END;
    pthread_exit(ret);
}

UINT32 thr_ncore()
{
	INT32 ncore = sysconf( _SC_NPROCESSORS_ONLN );
	return (ncore <= 0 ) ? 1 : ncore;
}

/// /// ///
/// JOB ///
/// /// ///

JOB thr_job_new(INT32 nthread, THRCALL thrcall, UINT32 stksz)
{
    _JOB* j= (_JOB*)malloc(sizeof(JOB));

    j->n = nthread;

    j->j = (THR*)malloc(sizeof(THR)*j->n);
    INT32 i;
    for (i=0 ; i < j->n ; i++)
    {
        j->j[i] = thr_new(thrcall,stksz,1,0);
        j->j[i]->param = NULL;
    }

    return j;
}

VOID thr_job_run(JOB j)
{
    INT32 i;
    for (i = 0; i < j->n; i++)
    {
        thr_run(j->j[i],j->j[i]->param);
    }
}

VOID thr_job_free(JOB j)
{
    INT32 i;
    for (i = 0 ; i < j->n ; i++)
    {
        thr_free(j->j[i]);
    }
    free(j->j);
    free(j);
}

VOID thr_job_wait(JOB j)
{
    INT32 i;
    for (i = 0; i < j->n; i++)
    {
        thr_waitthr(j->j[i]);
    }
}

INT32 thr_job_stop(JOB j, UINT32 ms, INT32 forceclose)
{
    INT32 i;
    for (i = 0; i < j->n ; i++)
    {
        if (!thr_stop(j->j[i],ms,forceclose))
            return 0;
    }

    return 1;
}

VOID thr_job_setparam(JOB j, UINT32 index, VOID* p)
{
    if (index >= j->n) return;
    j->j[index]->param = p;
}

