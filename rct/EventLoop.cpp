#define HAVE_LIBEVENT2
#include "EventLoop.h"
#include "Timer.h"
#include "rct-config.h"
#include <algorithm>
#include <atomic>
#include <set>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#if defined(HAVE_EPOLL)
#  include <sys/epoll.h>
#elif defined(HAVE_KQUEUE)
#  include <sys/event.h>
#  include <sys/time.h>
#endif

#include <event2/event.h>
#include <event2/thread.h>

#ifdef HAVE_MACH_ABSOLUTE_TIME
#  include <mach/mach.h>
#  include <mach/mach_time.h>
#endif

// this is pretty awful, any better ideas to avoid the unused warning?
#define STRERROR_R(errno, buf, size) if (strerror_r(errno, buf, size))

EventLoop::WeakPtr EventLoop::mainLoop;
std::mutex EventLoop::mainMutex;
static std::atomic<int> mainEventPipe;
static std::once_flag mainOnce;
static pthread_key_t eventLoopKey;

// sadly GCC < 4.8 doesn't support thread_local
// fall back to pthread instead in order to support 4.7

// ### static leak, one EventLoop::WeakPtr for each thread
// ### that calls EventLoop::eventLoop()
static EventLoop::WeakPtr& localEventLoop()
{
    EventLoop::WeakPtr* ptr = static_cast<EventLoop::WeakPtr*>(pthread_getspecific(eventLoopKey));
    if (!ptr) {
        ptr = new EventLoop::WeakPtr;
        pthread_setspecific(eventLoopKey, ptr);
    }
    return *ptr;
}

#define eintrwrap(VAR, BLOCK)                   \
    do {                                        \
        VAR = BLOCK;                            \
    } while (VAR == -1 && errno == EINTR)

static void signalHandler(int sig, siginfo_t *siginfo, void *context)
{
    char b = 'q';
    int w;
    const int pipe = mainEventPipe;
    if (pipe != -1)
        eintrwrap(w, ::write(pipe, &b, 1));
}

void eventSIGINTcb(evutil_socket_t fd, short what, void *userdata)
{
  if ( what & EV_SIGNAL )
    std::cout << "Caught signal: " << fd << std::endl;
}

EventLoop::EventLoop()
    : nextTimerId(0), stop(false), exitCode(0), execLevel(0), flgs(0)
{
    std::call_once(mainOnce, [](){
            mainEventPipe = -1;
            pthread_key_create(&eventLoopKey, 0);
            signal(SIGPIPE, SIG_IGN);
        });
}

EventLoop::~EventLoop()
{
    assert(!execLevel);
    cleanup();
}

void EventLoop::init(unsigned flags)
{
    std::lock_guard<std::mutex> locker(mutex);
    flgs = flags;
    evthread_use_pthreads();
    eventBase = event_base_new();
    auto sigEvent = evsignal_new(eventBase, SIGINT, eventSIGINTcb, NULL);
    
    std::shared_ptr<EventLoop> that = shared_from_this();
    localEventLoop() = that;
    if (flags & MainEventLoop)
        mainLoop = that;
}

void EventLoop::cleanup()
{
  std::cout << __PRETTY_FUNCTION__ << "\n";
  
  std::lock_guard<std::mutex> locker(mutex);
  localEventLoop().reset();

  while (!events.empty()) {
    delete events.front();
    events.pop();
  }

  if ( eventBase ) {
    event_base_free( eventBase );
    eventBase = nullptr;
  }
}

EventLoop::SharedPtr EventLoop::eventLoop()
{
    EventLoop::SharedPtr loop = localEventLoop().lock();
    if (!loop) {
        std::lock_guard<std::mutex> locker(mainMutex);
        loop = mainLoop.lock();
    }
    return loop;
}


void EventLoop::error(const char* err)
{
    fprintf(stderr, "%s\n", err);
    abort();
}

extern "C" void postCallback(evutil_socket_t fd,
			     short w,
			     void *data)
{
  EventLoop *evl = (EventLoop *)data;
  std::cout << __PRETTY_FUNCTION__ << " on fd = " << fd << "\n";
  evl->sendPostedEvents();
}

void EventLoop::post(Event* event)
{
  std::cout << __PRETTY_FUNCTION__ << std::endl;
  std::lock_guard<std::mutex> locker(mutex);
  events.push(event);

  event->mEvent =
    event_new( eventBase, -1, 0, postCallback, this );

  timeval tv = { 0, 1 * 100l };
  event_add( event->mEvent, &tv );
    
  wakeup();
}

void EventLoop::wakeup()
{
  std::cout << __PRETTY_FUNCTION__ << std::endl;
  
  if (std::this_thread::get_id() == threadId)
    return;

  // TODO: Perhaps force running of event ?? Dunno wtf this is for
}

void EventLoop::quit(int code)
{
  std::cout << __PRETTY_FUNCTION__ << std::endl;
  
  std::lock_guard<std::mutex> locker(mutex);


  if (!event_base_got_exit( eventBase )) {
    timeval tv = { 0, 1 * 1000l };
    event_base_loopexit( eventBase, &tv );
  }

  if (std::this_thread::get_id() == threadId) {
    if (execLevel) {
      stop = true;
      exitCode = code;
    }
    return;
  }

  if (execLevel && !stop) {
    stop = true;
    exitCode = code;
    wakeup();
  }
}

inline bool EventLoop::sendPostedEvents()
{
    std::unique_lock<std::mutex> locker(mutex);
    if (events.empty())
        return true;
    while (!events.empty()) {
        auto event = events.front();
        events.pop();
        locker.unlock();
	std::cout << "Sending Posted Event..." << std::endl;
        event->exec();
        delete event;
        locker.lock();
    }
    return true;
}

// milliseconds
static inline uint64_t currentTime()
{
#if defined(HAVE_CLOCK_MONOTONIC_RAW) || defined(HAVE_CLOCK_MONOTONIC)
    timespec now;
#if defined(HAVE_CLOCK_MONOTONIC_RAW)
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) == -1)
        return 0;
#elif defined(HAVE_CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return 0;
#endif
    const uint64_t t = (now.tv_sec * 1000LLU) + (now.tv_nsec / 1000000LLU);
#elif defined(HAVE_MACH_ABSOLUTE_TIME)
    static mach_timebase_info_data_t info;
    static bool first = true;
    uint64_t t = mach_absolute_time();
    if (first) {
        first = false;
        mach_timebase_info(&info);
    }
    t = t * info.numer / (info.denom * 1000); // microseconds
    t /= 1000; // milliseconds
#else
#error No time getting mechanism
#endif
    return t;
}

void EventLoop::eventDispatch(evutil_socket_t fd, short what, void *arg)
{
  EventCallbackData *cbd = reinterpret_cast<EventCallbackData *>( arg );
  if (!cbd || !cbd->evloop )
    return;

  cbd->evloop->dispatch( cbd->ev );
}

void EventLoop::dispatch(event *ev)
{
  std::function<void(int)> cb;
  {
    std::lock_guard<std::mutex> locker(mutex);
    auto it = eventCbMap.find( ev );
    if ( it == std::end(eventCbMap) ) {
      std::cout << "Event not found in Callback Map!\n";
      return;
    }

    cb = it->second;
  }

  if (!cb)
    std::cout << "Warning! cb null?!\n";
  else
    cb( 0 );

  //std::cout << __PRETTY_FUNCTION__ << " : timer fired -> dispatching cb\n";
  
  cb( 0 );
}

// timeout (ms)

int EventLoop::registerTimer(std::function<void(int)>&& func, int timeout, int flags)
{
  std::cout << "Register Timer Request! flags = " << flags << "\n";
  std::lock_guard<std::mutex> locker(mutex);

  timeval tv { 0, timeout * 1000l };

  short tflags = flags == Timer::SingleShot ? 0 : EV_PERSIST;

  std::unique_ptr<EventCallbackData> data( new EventCallbackData );
  
  event *etimer = event_new( eventBase, -1, tflags,
			     eventDispatch,
			     data.get() );

  data->evloop = this;
  data->ev = etimer;

  eventCbDataMap[ etimer ] = std::move( data );
  
  auto id = nextTimerId;
    
  eventCbMap[ etimer ] = func;
  idEventMap[ id ] = etimer;
    
  ++nextTimerId;

  evtimer_add( etimer, &tv );

  wakeup();
  return id;
}

void EventLoop::unregisterTimer(int id)
{
    // rather slow
    std::lock_guard<std::mutex> locker(mutex);

    auto idev_it = idEventMap.find( id ); 
    if (idev_it == std::end(idEventMap)) {
      std::cout << "Event ID " << id << " not Found!\n";
      return;
    }

    idEventMap.erase( idev_it );
    
    auto evcb_it = eventCbMap.find( idev_it->second );
    auto cb_it = eventCbDataMap.find( idev_it->second );

    event_del( idev_it->second );
    event_free( idev_it->second );

    if (evcb_it == std::end(eventCbMap)) {
      std::cout << "Event Not found in EventCB Map!\n";
    } else 
      eventCbMap.erase( evcb_it );

    if (cb_it == std::end(eventCbDataMap))
      std::cout << "Event's Callback Data not found in Map!\n";
    else
      eventCbDataMap.erase( cb_it );
}

inline bool EventLoop::sendTimers()
{ return false; }

void EventLoop::socketEventCB(evutil_socket_t fd, short what, void *arg)
{
  /*
  int err;
  socklen_t size = sizeof(err);
  e = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &size);
  if (e == -1) {
    STRERROR_R(errno, buf, sizeof(buf));
    fprintf(stderr, "Error getting error for fd %d: %d (%s)\n", fd, errno, buf);
  } else {
    STRERROR_R(errno, buf, sizeof(buf));
    fprintf(stderr, "Error on socket %d, removing: %d (%s)\n", fd, err, buf);
  }
  */

  auto mode = ((  what & EV_READ ? SocketRead : 0)
	       | (what & EV_WRITE ? SocketWrite : 0));
  typename SocketMap::iterator sockcb_it;
  
  {
    std::lock_guard<std::mutex> locker(mutex);

    sockcb_it = sockets.find( fd );
    // bool found = sockcb_it == std::end(sockets) ? false : true;
    // std::cout << "socket cb iter found = "
    // 	      << std::boolalpha
    // 	      << found  << "\n";
    if ( sockcb_it == std::end( sockets ))
      return;

  }
  sockcb_it->second.second( fd, mode );
}
void socket_event_cb( evutil_socket_t fd, short what, void *arg )
{
  EventLoop *evl = (EventLoop *)arg;
  // std::cout << "Socket Event CB hit! fd = " << fd
  // 	    << " evl = " << evl << "\n";
  evl->socketEventCB( fd, what, arg );
}

void EventLoop::registerSocket(int fd, int mode, std::function<void(int, int)>&& func)
{
  std::cout << "Register Socket Request! fd = " << fd << "\n";
  
  std::lock_guard<std::mutex> locker(mutex);
  sockets[fd] = std::make_pair(mode, std::forward<std::function<void(int, int)> >(func));

  auto what = (( mode & SocketRead ? EV_READ : 0 )
	       | (mode & SocketWrite ? EV_WRITE : 0)
	       | (mode & SocketOneShot ? 0 : EV_PERSIST ));

  auto ret = evutil_make_socket_nonblocking( fd );
  std::cout << "Make Socket Nonblocking: " << ret << "\n";

  auto socketEvent = event_new( eventBase, fd, what,
				socket_event_cb,
				this );

  socketEventMap[ fd ] = socketEvent;

  event_add( socketEvent, nullptr );
}

void EventLoop::updateSocket(int fd, int mode)
{
    std::cout << __PRETTY_FUNCTION__ << " new mode = " << mode << "\n";
    std::lock_guard<std::mutex> locker(mutex);
    std::map<int, std::pair<int, std::function<void(int, int)> > >::iterator socket = sockets.find(fd);
    if (socket == sockets.end()) {
        fprintf(stderr, "Unable to find socket to update %d\n", fd);
        return;
    }

    auto sockev_it = socketEventMap.find( fd );
    if ( sockev_it == std::end(socketEventMap) ) {
      std::cerr << "Unable to find event from socket fd " << fd << "\n";
      return;
    }

    event_free( sockev_it->second );
    socket->second.first = mode;

    // TODO: fuck copy/paste from registerSocket
    auto what = (( mode & SocketRead ? EV_READ : 0 )
		 | (mode & SocketWrite ? EV_WRITE : 0)
		 | (mode & SocketOneShot ? 0 : EV_PERSIST ));
    
    auto socketEvent = event_new( eventBase, fd, what,
				  socket_event_cb,
				  this );

    socketEventMap[ fd ] = socketEvent;
    event_add( socketEvent, nullptr );
}

void EventLoop::unregisterSocket(int fd)
{
  std::cout << __PRETTY_FUNCTION__ << " : fd = " << fd << "\n";
  std::lock_guard<std::mutex> locker(mutex);
    auto socket = sockets.find(fd);
    if (socket == sockets.end())
        return;

    auto sockev_it = socketEventMap.find( fd );
    if (sockev_it == std::end(socketEventMap))
      return;

    event_del( sockev_it->second );
    event_free( sockev_it->second );
    sockets.erase(socket);
    socketEventMap.erase( sockev_it );
}

int EventLoop::exec(int timeoutTime)
{
  bool exit = false;
  int ret;
  
  {
    std::lock_guard<std::mutex> locker(mutex);
    ++execLevel;
  }

  while (!exit) {

    if ( event_base_got_exit( eventBase ) ) {
      exit = true;
      continue;
    }
    
    else if (timeoutTime != -1) {
      // register a timer that will quit the event loop
      //registerTimer(std::bind(&EventLoop::quit, this, Timeout), timeoutTime, Timer::SingleShot);
      timeval tv = { 0, timeoutTime * 1000l };
      event_base_loopexit( eventBase, &tv );
      exit = true;
    } 
  
    if (!sendPostedEvents())
      return GeneralError;

    //ret = event_base_dispatch( eventBase );
    ret = event_base_loop( eventBase, EVLOOP_NONBLOCK );
    if ( ret == -1 )
      std::cout << "ERROR: EventLoop dispatch ret = " << ret << "\n";

    //std::cout << "event_base_dispatch returned!\n";
  }

  std::cout << "Exiting EventLoop!\n";
  std::cout << "Events, Active: "
	    << event_base_get_num_events( eventBase, EVENT_BASE_COUNT_ACTIVE )
	    << " Virtual: "
	    << event_base_get_num_events( eventBase, EVENT_BASE_COUNT_VIRTUAL )
	    << " Added: "
	    << event_base_get_num_events( eventBase, EVENT_BASE_COUNT_ADDED )
	    << "\n";

  cleanup();
  
  return ret == 1 ? Success : Timeout ;
}
