
/// picomsg
/// Licence: https://creativecommons.org/licenses/by/4.0/

/// Simple parent-child queue-based message-passing.
/// Uses two threads, and is non-blocking.

/// Released 2024
/// Author: Theodore H. Smith, http://gamblevore.org


#ifndef __PICO_MSG__
#define __PICO_MSG__

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
#ifndef PicoDefaultInitSize
	#define PicoDefaultInitSize (1024*1024)
#endif
#ifndef PicoParentSocketNum
	#define PicoParentSocketNum (987)
#endif
#include <stdint.h> // for picodate

typedef int64_t	PicoDate;  // 16 bits for small stuff

struct			PicoComms;
struct			PicoMessage { int Length;  char* Data;  operator bool () {return Data;}; };

struct 			PicoConfig  { const char* Name;  PicoDate LastRead;  int Noise;  float SendTimeOut;  int SendFailed;  int ReadFailed;  int QueueBytesRemaining;  int Bits; };

typedef void* (*PicoThreadFn)(PicoComms* M);

#ifndef PICO_IMPLEMENTATION
	#define _pico_code_(x) ;
#else
	#define _pico_code_(x) {x}

	#include <poll.h>
	#include <fcntl.h>
	#include <stdio.h>
	#include <unistd.h>
	#include <string.h>
	#include <pthread.h>
	#include <sched.h>
	#include <errno.h>
	#include <sys/socket.h>
	#include <algorithm>
	#include <deque>
	#include <arpa/inet.h>
	#include <atomic>


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

extern "C" bool pico_dup2(int from, int to) { // so this kinda does what dup2 should do.
	while (from > 0 and dup2(from, to) == -1) {
		int err = errno;
		if (err != EINTR and err != EBUSY)	
			return false;
	}
	return true;
}

extern "C" int pico_dup(int from) { // this is what dup should do.
	int to = -1;
	while ((to = dup(from)) == -1) {
		int err = errno;
		if (err != EINTR and err != EBUSY)	
			break;
	}
	return to;
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
static	void*				pico_worker (PicoComms* Dummy);
static	std::atomic_int		pico_thread_count;
static	int					pico_desired_thread_count = 2;
static	const char*			pico_fail_actions[4] = {"Failed", "Reading", "Sending", 0};
int							pico_start (PicoComms* ErrorPlace);

struct PicoBuff {
	const char*			Name;
	std::atomic_uint	Tail;
	std::atomic_uint	Head;
	int					Size;
	std::atomic_int		RefCount;	
	PicoTrousers		WorkerThread;
	PicoComms*			Owner;
	char				SectionStart[0];

	static PicoBuff* New (int bits, const char* name, PicoComms* O) { // 🕷️
		PicoBuff* Rz = nullptr; bits++; 
		while (!Rz) {
			bits--; 
			if (bits < 10) return nullptr;
			if ((Rz = (PicoBuff*)calloc((1<<bits)+sizeof(PicoBuff), 1))) break;
		}
		Rz->Size = 0; Rz->Tail = 0; Rz->Head = 0; Rz->Owner = O; Rz->RefCount = 1;
		Rz->Size = 1<<bits; Rz->Name = name;
		return Rz;
	}
	
	void Decr () {
		if (--RefCount == 0)
			free(this);
	}
		
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
	int							Socket;
	unsigned char				Err;
	char						HalfClosed;
	bool						IsParent;
	PicoTrousers				QueueLocker;
	int							LengthBuff;
	std::deque<PicoMessage>		TheQueue;
	PicoBuff*					Reading;
	PicoBuff*					Sending;
	
	PicoComms (int noise, bool isparent, int Size) {
		RefCount = 1; Socket = 0; Err = 0; IsParent = isparent; HalfClosed = 0; LengthBuff = 0; Sending = 0; Reading = 0;
		memset(&Conf, 0, sizeof(PicoConfig)); 
		Conf.Noise = noise;
		Conf.SendTimeOut = 10.0f;

		int B = 31 - __builtin_clz(Size);
		if (B < 10) B = 10;
		B += (1<<B < Size);
		Conf.Bits = B;
		Conf.QueueBytesRemaining = 1<<(B+3);
	}

	~PicoComms () { 
		SayEvent("Deleted");
		really_close();
		for (auto& M:TheQueue)
			free(M.Data);
		if (Sending) Sending->Decr();
		if (Sending) Reading->Decr();
	}

	PicoComms* InitPair (int Noise) {
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return nullptr;
		PicoComms* Rz = new PicoComms(Noise, false, 1<<Conf.Bits);
		add_conn(Socks[0]);
		Rz->add_conn(Socks[1]);
		return Rz;
	}

	bool InitThread (int Noise, PicoThreadFn fn) {
		if (!alloc_buffs()) return false;
		PicoComms* C = new PicoComms(Noise, false, 0);
		Sending->RefCount++;     Reading->RefCount++;  Socket = -1;   
		C->Sending = Reading; C->Reading = Sending; C->Socket = -1;
		
		add_sub();
		C->add_sub();
		return thread_run(C, fn);
	}
	
	bool InitSocket (int Sock) {
		IsParent = false;
		return (Sock > 0 or failed(EBADF)) and add_conn(Sock);
	}

	bool InitExec (int Sock) {
		int NewSock = pico_dup(Sock); // free up the number. In case we wanna exec all over again.
		close(Sock);
		return InitSocket(NewSock); 
	}

	pid_t InitFork (bool WillExec) {
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return -errno;
		pid_t pid = fork();
		if (pid < 0) return -errno;

		IsParent = pid!=0; // 🕷️_🕷️
		close(Socks[!IsParent]);
		int S = Socks[IsParent];
		if (!IsParent) {
			pico_thread_count = 0; // Unix doesn't let us keep threads.
			if (WillExec) {
				pico_dup2(S, PicoParentSocketNum);
				close(S); S = PicoParentSocketNum;
				return pid;
			}
		} 

		add_conn(S);
		return pid; 
	}
	
	bool StillSending () {
		return !(HalfClosed & 2) and Sending->Length() > 0;	
	}
	
	bool QueueSend (const char* msg, int n, int Policy) {
		if (!msg or n <= 0 or HalfClosed&2 or !Sending) return false;
		if (queue_sub(msg, n)) return true;
		if (n+4 > Sending->Size)
			return SayEvent("CantSend: Message too large!");
		if (Policy == PicoSendGiveUp)
			return (!Conf.SendFailed++) and SayEvent("CantSend: BufferFull");
		
		PicoDate Final = PicoGetDate() + (PicoDate)(Conf.SendTimeOut*65536.0f);
		while (PicoGetDate() < Final) {
			if (HalfClosed&2) return false; // closed!
			sched_yield();
			if (queue_sub(msg, n)) return true;
		}
		
		return (!Conf.SendFailed++) and SayEvent("CantSend: TimedOut");
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
		const char* P = Conf.Name;
		P = P?P:(IsParent ? "Parent" : "Child");
		if (Iter)
			printf("%s.%s: %s %s %i\n", S, P, A, B, Iter);
		  else
			printf("%s.%s: %s %s\n", S, P, A, B);
		
		return nullptr;
	}
	
	void AskClose () {
		if (HalfClosed>=3) return;

		if (!Err) Err = ENOTCONN;
		report_closed_buffers();
		SayEvent("Disconnecting");
	}
	
	void Destroy () {
		AskClose(); decr();
	}


//// INTERNALS ////
	
	bool queue_sub (const char* msg, int n) {
		return Sending and Sending->AppendMsg(msg, n); // in case I wanna put debug tests here.
	}

	void do_reading () {
		if (HalfClosed&1) return;
		if (!Reading->WorkerThread.enter()) return;
		if (Socket < 0) // memory only
			while (acquire_msg()) {;}
		  else
			while ( auto Msg = Reading->AskUnused() ) {
				if (HalfClosed&1) break; 
				int Amount = (int)recv(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
				if (Amount > 0) {
					Reading->gained(Amount);
					if (CanSayDebug()) Say("|recv|", "", Amount);
					while (acquire_msg()) {;}
				} else if (!io_pass(Amount, 1))
					break;
			}
		Reading->WorkerThread.unlock();
	}
	
	void do_sending () {
		int Remain = Sending->Length();
		if (HalfClosed&2 or !Remain or Socket < 0) return;
		
		if (!Sending->WorkerThread.enter()) return;
		
		while ( auto Msg = Sending->AskUsed() ) { // send(MSG_DONTWAIT) does nothing on OSX. but we set non-blocking anyhow.
			int Amount = (int)send(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
  			if (Amount > 0) {
				Sending->lost(Amount);
				if (CanSayDebug()) Say("|send|", "", Amount);
			} else if (!io_pass(Amount, 2))
				break;
		}
		Sending->WorkerThread.unlock();
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
			if (Reading->Length() < 4) return false;
			Reading->Get((char*)&LengthBuff, 4);
			L = LengthBuff = ntohl(LengthBuff); 
		}
			
		if (L <= 0)
			return failed(EILSEQ);

		if (Reading->Length() < L) return false;
		int QS = L + sizeof(PicoMessage)*2;
		if (Conf.QueueBytesRemaining < QS)
			return (!Conf.ReadFailed++) and SayEvent("CantRead: BufferFull");
		
		if (char* Data = (char*)malloc(L); Data) {
			Reading->Get(Data, L);
			LengthBuff = 0;
			Conf.QueueBytesRemaining -= QS;
			QueueLocker.lock();
			TheQueue.push_back({L, Data});
			QueueLocker.unlock();
			Conf.LastRead = PicoGetDate();
			return true;
		}
		return failed(ENOBUFS);
	}
	
	bool get_pair_of (int* Socks) {
		return  socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks)==0  or  failed();
	}
	
	bool alloc_buffs () {
		if (!Sending)
			Sending = PicoBuff::New(Conf.Bits, "Send", this);
		if (!Reading)
			Reading = PicoBuff::New(Conf.Bits, "Read", this);

		if (Reading and Sending and pico_start(this)) return true;
		
		return really_close() or failed(ENOBUFS);
	}

	bool add_conn (int Sock) {
		int FL = fcntl(Sock, F_GETFL, 0);
		if (FL == -1) return failed() or Say("Bad Socket", "", Sock);
		if (!alloc_buffs()) return false;
		Socket = Sock;
		fcntl(Socket, F_SETFL, FL | O_NONBLOCK);
		return add_sub();
	}

	bool add_sub () {
		pico_list.AddComm(this);
		return !SayEvent("Started");
	}
	
	static bool thread_run (PicoComms* C, PicoThreadFn fn) {
		pthread_t T = 0;   ;;;/*_*/;;;   // creeping upwards!!
		if (!pthread_create(&T, nullptr, (void*(*)(void*))fn, C) and !pthread_detach(T))
			return true;
		return !C or C->Say("Thread Failed");
	}
	
	void report_closed_buffers () {
		if (CanSayDebug() and HalfClosed != -1 ) {
			int SL = Sending->Length();
			int RL = Reading->Length();
			if (SL) Say("Sending", "Still Contains", SL);
			Say("Sent", "", Sending->Tail);
			if (RL) Say("Reading", "Still Contains", RL);
			Say("Read", "", Reading->Head);
		}
		HalfClosed = -1;
	}

	void decr () {
		if (RefCount-- == 1)
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
		return nullptr;
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
		int S = Socket; if (!S) return false;
		Socket = 0; 
		if (S > 0) close(S);
		report_closed_buffers();
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

int pico_start (PicoComms* ErrorPlace) {
	for (int i = pico_thread_count; i < pico_desired_thread_count; i++)
		if (!PicoComms::thread_run(ErrorPlace, pico_worker))
			return i;

	return pico_desired_thread_count;
}    ;;;/*_*/;;;  ;;;/*_*/;;;     ;;;/*_*/;;;   // more spiders


static void* pico_worker (PicoComms* Dummy) {
	pico_thread_count++;
	pico_last_activity = PicoGetDate(); // get to work!
	while (true) pico_work_comms();
	pico_thread_count--; // nice
}

#endif



/// C-API ///
/// **Initialisation / Destruction** ///
extern "C" PicoComms* PicoCreate ()  _pico_code_ (
	return new PicoComms(PicoNoiseEvents, true, PicoDefaultInitSize);
)

extern "C" PicoComms* PicoStartChild (PicoComms* M) _pico_code_ (
	return M->InitPair(PicoNoiseEvents);
)

extern "C" bool PicoCompleteExec (PicoComms* M) _pico_code_ (
	return M->InitExec(PicoParentSocketNum);
)

extern "C" bool PicoStartThread (PicoComms* M, PicoThreadFn fn) _pico_code_ (
	return M->InitThread(PicoNoiseEvents, fn);
) 

extern "C" int PicoStartFork (PicoComms* M, bool WillExec=false) _pico_code_ (
	return M?M->InitFork(WillExec):fork();
)

extern "C" void PicoDestroy (PicoComms* M) _pico_code_ (
	if (M) M->Destroy();
)


/// **Communications** ///
extern "C" bool PicoSend (PicoComms* M, PicoMessage Msg, int Policy=PicoSendGiveUp) _pico_code_ (
	return M->QueueSend(Msg.Data, Msg.Length, Policy);
)

extern "C" bool PicoSendStr (PicoComms* M, const char* Msg, bool Policy=PicoSendGiveUp) _pico_code_ (
	return M->QueueSend(Msg, (int)strlen(Msg)+1, Policy);
)

extern "C" PicoMessage PicoGet (PicoComms* M, float Time=0) _pico_code_ (
	return M->Get(Time);
)


/// **Utilities** ///
extern "C" void PicoClose (PicoComms* M) _pico_code_ (
	M->AskClose();
)

extern "C" void* PicoSay (PicoComms* M, const char* A, const char* B="", int Iter=0) _pico_code_ (
	return M->Say(A, B, Iter);
)

extern "C" int PicoErr (PicoComms* M) _pico_code_ (
	return M?M->Err:-1;
)

extern "C" PicoConfig* PicoConf (PicoComms* M) _pico_code_ (
	return &M->Conf;
)

extern "C" bool PicoStillSending (PicoComms* M) _pico_code_ (
	return M->StillSending();
)

extern "C" int PicoSocket (PicoComms* M) _pico_code_ (
	return M?M->Socket:0;
)

bool PicoHasParent () _pico_code_ (
	return !(fcntl(PicoParentSocketNum, F_GETFL) == -1 and errno == EBADF);
)

bool PicoDesiredThreadCount (int C) _pico_code_ (
	pico_desired_thread_count = C;
	return !pico_list.Next or pico_start(nullptr);
)


#endif
