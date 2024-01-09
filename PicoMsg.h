
/// picomsg
/// Licence: https://creativecommons.org/licenses/by/4.0/

/// Simple parent-child queue-based message-passing.
/// Uses two threads, and is non-blocking.

/// Released 2024
/// Author: Theodore H. Smith, http://gamblevore.org

// future upgrades:
// Only use recv/send for internet, or if fork can't share memory, otherwise use same buffer. 


#ifndef __PICO_MSG__
#define __PICO_MSG__
#include <atomic>

#define PicoSilent				0
#define PicoNoiseDebugChild		1
#define PicoNoiseDebugParent	2
#define PicoNoiseDebug			3
#define PicoNoiseEventsChild	4
#define PicoNoiseEventsParent	8
#define PicoNoiseEvents			12
#define PicoNoiseAll			15
#define PicoSendGiveUp			0
#define PicoSendCanTimeOut		1
#ifndef PicoDesiredThreadCount
	#define PicoDesiredThreadCount	2
#endif

struct			PicoComms;
struct			PicoMessage { int Length; char* Data; operator bool () {return Data;}; };

struct 			PicoConfig  { const char* Name; int Noise; float SendTimeOut; int SendFullCount; int ReadFullCount; int QueueBytesRemaining; };

#ifndef PICO_IMPLEMENTATION
	#define _pico_code_(x) ;
#else
	#define _pico_code_(x) {x}

	#include <poll.h>
	#include <fcntl.h>
	#include <stdio.h>
	#include <unistd.h>
	#include <string>
	#include <pthread.h>
	#include <sched.h>
	#include <errno.h>
	#include <sys/socket.h>
	#include <algorithm>
	#include <deque>


struct PicoTrousers { // only one person can wear them at a time.
	std::atomic_bool Value;
	PicoTrousers  () {Value = false;}
	operator bool () {return Value;}
	void operator = (bool b) {Value = b;}
	bool enter () {
		bool Expected = false;
		return Value.compare_exchange_weak(Expected, true);
	}

	void unlock () {
		Value = false;
	}
	void lock () {
		while (!enter()); // spin
	}
};


typedef int64_t	PicoDate;  // 16 bits for small stuff
PicoDate pico_date_create ( uint64_t S, uint64_t NS ) {
	NS /= (uint64_t)15259; // for some reason unless we spell this out, xcode will miscompile this.
	S <<= 16;
	return S + NS;
}

PicoDate PicoGetDate( ) {
	timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	return pico_date_create(ts.tv_sec, ts.tv_nsec);
}

static void pico_sleep (float S) {
	S = std::clamp(S, 0.0001f, 0.9999f);
	timespec ts = {0, (int)(S*1000000000.0)};
	nanosleep(&ts, 0);
}



PicoTrousers PicoCommsLocker;
struct PicoCommsBase {
	std::atomic_int			RefCount;
	PicoCommsBase*			Next;
	PicoCommsBase*			Prev;
	PicoConfig				Conf;
	
	PicoCommsBase () { Prev = this; Next = 0;}
	
	void AddComm (PicoCommsBase* New) {
		PicoCommsLocker.lock();
		PicoCommsBase* N = Next;
		if (N)
			N->Prev = New;
		New->Next = N;
		New->Prev = this;
		Next = New;
		PicoCommsLocker.unlock();
	}
	
	;;;/*_*/;;;   // <-- a spider that ehlps you do async work
	void RemoveComm () {
		PicoCommsLocker.lock();
		PicoCommsBase* N = Next;
		PicoCommsBase* P = Prev;
		if (N)
			N->Prev = P;
		P->Next = N;
		Next = 0;
		Prev = 0;
		PicoCommsLocker.unlock();
	}
	
	PicoComms* NextComm () {
		PicoCommsLocker.lock();
		auto N = Next;
		if (N)
			N->RefCount++;
		PicoCommsLocker.unlock();
		return (PicoComms*)N;
	}
};


static	PicoCommsBase		pico_list;
static	PicoDate			pico_last_activity;
static	void*				pico_worker (void* obj);
static	std::atomic_int		pico_thread_count;
static	const char*			pico_fail_actions[4] = {"Failed", "Reading", "Sending", 0};


struct PicoBuff {
	const char*			Name;
	char*				SectionStart;
	std::atomic_uint	Tail;
	std::atomic_uint	Head;
	int					Size;
	PicoTrousers		WorkerThread;
	
	PicoBuff () {SectionStart = 0; Size = 0; Tail = 0; Head = 0; Name = "";}
	~PicoBuff () {free(SectionStart);}
	
	bool Alloc (int bits, const char* name) { // 🕷️
		Name = name;
		while (true) {
			if ((SectionStart = (char*)malloc(1<<bits))) break;
			bits--; 
			if (bits < 10) return false;
		}
		Head = 0; Tail = 0;
		Size = 1<<bits;
		return true;
	}
	
/*
	||||||||------- // start
	----||||------- // middle
	------||||||||| // end (partial)
	||||||||||||||| // end (whole)
	|||||-----||||| // overlapping
*/
	
	PicoMessage AskUsed () {
		int T = Tail; int H = Head; int S = Size; int B = S - 1;
		if (T >= H) return {};
		T &= B; H &= B; // 🕷️ / 🕷️
		if (T >= H) // tail to head... or to size
			H = S;
		return {H-T, SectionStart+T}; // 🕷️  🕷️ 🕷️
	}
	
	PicoMessage AskUnused () {
		int T = Tail; int H = Head; int S = Size; int B = S-1;
		int L = H - T;
		if (L >= S) return {};
		T &= B; H &= B;
		if (T > H) // head to size, or head to tail.
			S = T;
		return {S-H, SectionStart+H};
	}

	void lost (int N) {
		pico_last_activity = PicoGetDate();
		Tail += N;
	}

	void gained (int N) { 
		pico_last_activity = PicoGetDate();
		Head += N;
	}
	
	int Length () {
		return Head - Tail;
	}; 

	bool AppendMsg (const char* Src, int MsgLen) {
		int NetLen = htonl(MsgLen);
		if (Size - Length() >= MsgLen+4) {
			append_sub((char*)&NetLen, 4);
			append_sub(Src, MsgLen);
			return true;
		}
		return false;
	}
	
	void append_sub (const char* Src, int Need) {
		while (auto Dest = AskUnused()) {
			int Avail = std::min(Need, Dest.Length);
			memcpy(Dest.Data, Src, Avail);
			gained(Avail);
			Need -= Avail;
			Src += Avail;
			if (Need <= 0) return;
		};
	}
	
	bool Get (char* Dest, int N) {
		while (auto Msg = AskUsed()) {
			int B = std::min(Msg.Length, N); // 🕷️_🕷️
			memcpy(Dest, Msg.Data, B);
			Dest += B;
			N -= B; // 🕷️
			lost(B);
			if (N <= 0) break;
		}
		return true;
	}
}; 



struct PicoComms : PicoCommsBase {
	int						Socket;
	unsigned char			Err;
	char					HalfClosed;
	bool					IsParent;
	int						LengthBuff;
	std::deque<PicoMessage>	TheQueue;
	PicoTrousers			QueueLocker;
	PicoBuff				Reading;
	PicoBuff				Sending;
	
	PicoComms (int noise, bool isparent, int Size) {
		int Bits = 31 - __builtin_clz(Size);
		if (Bits < 10) Bits = 10;
		Bits += (1<<Bits < Size);
		RefCount = 1; Socket = 0; Err = 0; IsParent = isparent; HalfClosed = 0; LengthBuff = 0;
		memset(&Conf, 0, sizeof(PicoConfig)); 
		Conf.Name = isparent ? "Parent" : "Child";
		Conf.Noise = noise;
		Conf.SendTimeOut = 10.0f;
		Conf.QueueBytesRemaining = 1<<(Bits+3);
		if (!Sending.Alloc(Bits, "Send") or !Reading.Alloc(Bits, "Read"))
			failed(ENOBUFS);
	}
	
	~PicoComms () { 
		for (auto& M:TheQueue)
			free(M.Data);
		really_close();
		SayEvent("Deleted");
	}
//
	PicoComms* Pair (int Noise) {
		int Socks[2] = {};
		if (!pico_start() or !get_pair_of(Socks)) return 0;
		PicoComms* Rz = new PicoComms(Noise, false, Reading.Size);
		add_conn(Socks[0]);
		Rz->add_conn(Socks[1]);
		return Rz;
	}

	pid_t Fork () {
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return -errno;
		pid_t pid = fork();
		if (pid < 0) return -errno;

		IsParent = pid!=0;
		Conf.Name = IsParent?"Parent":"Child"; // 🕷️_🕷️
		close(Socks[!IsParent]);
		add_conn(Socks[IsParent]);

		if (!pico_start() and !IsParent) exit(-1);
		
		if (!IsParent)
			pico_thread_count = 0; // Unix doesn't let us keep threads.
		return pid; 
	}
	
	bool StillSending () {
		return !(HalfClosed & 2) and Sending.Length() > 0;	
	}
	
	bool QueueSend (const char* msg, int n, int Policy) {
		if (HalfClosed&2) return false;
		if (queue_sub(msg, n)) return true;
		if (n+4 > Sending.Size)
			return SayEvent("CantSend: Message too large!");
		if (Policy == PicoSendGiveUp)
			return (!Conf.SendFullCount++) and SayEvent("CantSend: BufferFull");
		
		PicoDate Final = PicoGetDate() + (PicoDate)(Conf.SendTimeOut*65536.0f);
		while (PicoGetDate() < Final) {
			if (HalfClosed&2) return false; // closed!
			sched_yield();
			if (queue_sub(msg, n)) return true;
		}
		
		return (!Conf.SendFullCount++) and SayEvent("CantSend: TimedOut");
	}
	
	PicoMessage Get (float T = 0.0) {
		if (!can_get(T)) return {};

		QueueLocker.lock();
		PicoMessage M = TheQueue.front();
		TheQueue.pop_front();
		QueueLocker.unlock();
		Conf.QueueBytesRemaining  +=  M.Length + sizeof(PicoMessage)*2;
		return M;
	}

	void* SayEvent (const char* A, const char* B="", int Iter=0) {
		if (Conf.Noise & (PicoNoiseEventsChild << IsParent))
			return Say(A, B, Iter);
		return nullptr;
	}

	inline bool CanSayDebug () {return Conf.Noise & (PicoNoiseDebugChild << IsParent);}
	
	void* Say (const char* A, const char* B="", int Iter=0) {
		const char* S = IsParent?"Us":"Them";
		
		if (Iter)
			printf("%s.%s: %s %s %i\n", S, Conf.Name, A, B, Iter);
		  else
			printf("%s.%s: %s %s\n", S, Conf.Name, A, B);
		
		return nullptr;
	}
	
	void AskClose () {
		if (HalfClosed>=3) return;

		if (!Err) Err = ENOTCONN;
		ReportClosedBuffers();
		SayEvent("Disconnecting");
	}
	
	void Destroy () {
		AskClose(); decr();
	}


//// INTERNALS ////
	
	bool queue_sub (const char* msg, int n) {
		return Sending.AppendMsg(msg, n); // in case I wanna put debug tests here.
	}

	void do_reading () {
		if (HalfClosed&1) return;
		if (!Reading.WorkerThread.enter()) return;
		
		while ( auto Msg = Reading.AskUnused() ) {
			if (HalfClosed&1) break; 
			int Amount = (int)recv(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
			if (Amount > 0) {
				if (CanSayDebug())
					Say("|recv|", "", Amount);
				Reading.gained(Amount);
				while (acquire_msg()) {;}
			} else if (!io_pass(Amount, 1))
				break;
		}
		Reading.WorkerThread.unlock();
	}
	
	void do_sending () {
		int Remain = Sending.Length();
		if (HalfClosed&2 or !Remain) return;
		
		if (!Sending.WorkerThread.enter()) return;
		
		while ( auto Msg = Sending.AskUsed() ) { // send(MSG_DONTWAIT) does nothing on OSX. but we set non-blocking anyhow.
			int Amount = (int)send(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
  			if (Amount > 0) {
				if (CanSayDebug()) Say("|send|", "", Amount);
				Sending.lost(Amount);
			} else if (!io_pass(Amount, 2))
				break;
		}
		Sending.WorkerThread.unlock();
	}
	
	bool can_get (float T) {
		if (!TheQueue.empty()) return true;
		if (!T) return false;
		if (T < 0) T = Conf.SendTimeOut;
		T = std::min(T, 54321000.0f); // 1.5 years?
		PicoDate Final = PicoGetDate() + (PicoDate)(T*65536.0f);
		timespec ts = {0, 1000000}; int n = T*16000;
		for ( int i = 0;  i < n and !(HalfClosed&1);   i++) {
			nanosleep(&ts, 0);
			if (!TheQueue.empty()) return true; 
			if (PicoGetDate() > Final) return false;
		}
		return false;
	}

	bool acquire_msg () {
		int L = LengthBuff;
		if (!L) {
			if (Reading.Length() < 4) return false;
			Reading.Get((char*)&LengthBuff, 4);
			L = LengthBuff = ntohl(LengthBuff); 
		}
			
		if (L <= 0)
			return failed(EILSEQ);

		if (Reading.Length() < L) return false;
		int QS = L + sizeof(PicoMessage)*2;
		if (Conf.QueueBytesRemaining < QS)
			return (!Conf.ReadFullCount++) and SayEvent("CantRead: BufferFull");;
		
		if (char* Data = (char*)malloc(L); Data) {
			Reading.Get(Data, L);
			LengthBuff = 0;
			Conf.QueueBytesRemaining -= QS;
			QueueLocker.lock();
			TheQueue.push_back({L, Data});
			QueueLocker.unlock();
			return true;
		}
		return failed(ENOBUFS);
	}
	
	bool get_pair_of (int* Socks) {
		return  socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks)==0  or  failed();
	}
	
	void add_conn (int Sock) {
		Socket = Sock;
		fcntl(Socket, F_SETFL, fcntl(Sock, F_GETFL, 0) | O_NONBLOCK);
		pico_list.AddComm(this);
		SayEvent("Started");
	}
	
	bool pico_start () {
		pthread_t T = 0;
		for (int i = pico_thread_count; i < PicoDesiredThreadCount; i++)
			if (pthread_create(&T, 0, pico_worker, 0) or pthread_detach(T))
				if (i == 0)
					return !Say("Thread Failed") and really_close(); // 1 thread is still OK.

		return true;
	}    ;;;/*_*/;;;  ;;;/*_*/;;;     ;;;/*_*/;;;   // more spiders


	void ReportClosedBuffers () {
		if (CanSayDebug() and HalfClosed != -1 ) {
			if (Sending.Length()) Say("Sending", "Still Contains", Sending.Length());
			Say("Sent", "", Sending.Tail);
			if (Reading.Length()) Say("Reading", "Still Contains", Reading.Length());
			Say("Read", "", Reading.Head);
		}
		HalfClosed = -1;
	}

	void decr () {
		if (!--RefCount)
			delete this;
	}

	void* failed (int err=errno, int Action=0) {
		if (!Err) {
			if (err == EPIPE and HalfClosed) {
				err = ENOTCONN;
				SayEvent("ClosedGracefully");
			} else {
				SayEvent(pico_fail_actions[Action], strerror(err));
			}
		}
		Err = err;
		HalfClosed = 3;
		return 0;
	}
		
	bool io_pass (int Amount, int Half) {
		if (!Amount) {
			HalfClosed |= Half;
			return (HalfClosed >= 3) and failed(EPIPE, Half);
		}
		int e = errno;
		if (e == EAGAIN) return false;
		if (e == EINTR)  return true;
		return failed(e, Half);
	}
	
	bool really_close() {
		if (!Socket) return false; 
		Socket = close(Socket)&0;
		ReportClosedBuffers();
		RemoveComm();
		return CanSayDebug() and Say("Closing");
	}

	PicoComms* io () {
		auto N = NextComm();
		if (Socket) {
			do_reading();
			do_sending();
			if (HalfClosed>=3) really_close();
		}
		decr(); 
		return N;
	}
};



static void pico_work_comms () {
	auto M = pico_list.NextComm();
	while (M)
		M = M->io();
	
	float S = (PicoGetDate() - pico_last_activity) * 0.000015258789f; // seconds
	S = std::clamp(S, 0.03f, 0.9999f);
	S = S*S;
	pico_sleep(S);
}


static void* pico_worker (void* T) {
	pico_thread_count++;
	pico_last_activity = PicoGetDate(); // get to work!
	while (true) pico_work_comms();
	pico_thread_count--; // nice
}

#endif



/// C-API ///
/// **initialisation / destruction** ///
extern "C" PicoComms* PicoMsgComms (int Noise=PicoNoiseEvents, int Size=1024*1024)  _pico_code_ (
	return new PicoComms(Noise, true, Size);
)

extern "C" PicoComms* PicoMsgCommsPair (PicoComms* M, int Noise=PicoNoiseEvents) _pico_code_ (
	return M->Pair(Noise);
)

extern "C" int PicoMsgFork (PicoComms* M) _pico_code_ (
	return M->Fork();
)

extern "C" void PicoMsgDestroy (PicoComms* M) _pico_code_ (
	if (M) M->Destroy();
)

/// **communications** ///
extern "C" bool PicoMsgSend (PicoComms* M, PicoMessage Msg, int Policy=PicoSendGiveUp) _pico_code_ (
	return (Msg and Msg.Length > 0) and M->QueueSend(Msg.Data, Msg.Length, Policy);
)

extern "C" bool PicoMsgSendStr (PicoComms* M, const char* Msg, float Time=0) _pico_code_ (
	return M->QueueSend(Msg, (int)strlen(Msg)+1, Time);
)

extern "C" PicoMessage PicoMsgGet (PicoComms* M, float Time=0) _pico_code_ (
	return M->Get(Time);
)


/// **utilities** ///
extern "C" void PicoMsgClose (PicoComms* M) _pico_code_ (
	M->AskClose();
)

extern "C" void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0) _pico_code_ (
	return M->Say(A, B, Iter);
)

extern "C" int PicoMsgErr (PicoComms* M) _pico_code_ (
	return M->Err;
)

extern "C" PicoConfig* PicoMsgConf (PicoComms* M) _pico_code_ (
	return &M->Conf;
)

extern "C" bool PicoMsgStillSending (PicoComms* M) _pico_code_ (
	return M->StillSending();
)

#endif
