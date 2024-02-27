
/// picomsg
/// Licence: https://creativecommons.org/licenses/by/4.0/

/// Simple parent-child queue-based message-passing.
/// Uses two threads, and is non-blocking.
/// 64 communicators max, this can be increased in the future. 

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

#include <stdint.h> // for picodate

typedef int64_t	PicoDate;  // 16 bits for small stuff

struct			PicoComms;
struct			PicoMessage { char* Data; int Length;   operator bool () {return Data;}; };

struct 			PicoConfig  {
	const char* Name;			/// Used for reporting events to stdout.
	PicoDate	LastRead;		/// The date of the last Read. This is a signed 64-bit number. The lower 16-bits used for sub-second resolution, and the upper 47-bits are for seconds. 1 bit used for the sign.
	PicoDate	LastSend;		/// The date of the last send.
	int 		Noise;			/// How much printing to stdout that PicoMsg does. Anything from PicoSilent to PicoNoiseAll.
	float		SendTimeOut;	/// The number of seconds before a send will timeout (if the send is not instant)
	int			SendFailCount;	/// How many time sending failed.
	int			ReadFailCount;	/// How many times reading failed.
	int			QueueSize;		/// The allowed combined-size for unread messages. There is no hard limit. 8MB default.
	int			Bits;			// The buffer-size in 1<<Bits. must be set before calling `PicoStart...()`
};

typedef bool (*PicoThreadFn)(PicoComms* M, void* self, const char** Args);

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
	#include <arpa/inet.h>
	#include <atomic>
	#include <execinfo.h>
	#include <charconv>
	#include <signal.h>


extern "C" bool				PicoStart (int Suicide);

static PicoMessage pico_next_msg (PicoMessage M) {
	return *((PicoMessage*)(M.Data + M.Length));
}


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



static PicoTrousers PicoCommsLocker;
	
inline int pico_log2 (uint64_t X) {
	return 63-__builtin_clzll(X);
}

struct PicoCommList {
	std::atomic_uint64_t		Map;
	PicoTrousers				Lock;
	PicoComms*					Items[64];
	operator bool () { return Map!=-1; }
	
	int Add (PicoComms* M) {
		if (Map+1) {
			Lock.lock();
			uint64_t F0 = Map;
			auto F = ~F0;
			F &= -F;
			int ID = pico_log2(F);
			Items[ID] = M;
			Map = F|F0;
			Lock.unlock();
			return ID;
		}
		
		puts("pico: TooManyComms");
		return -1;
	}
	
	void Remove (int64_t M) {
		Lock.lock();
		Items[M] = 0;
		M = 1 << M;
		Map &=~ M;
		Lock.unlock();
	}
};


static	PicoCommList			pico_list;
static	PicoDate				pico_last_activity;
static	std::atomic_int			pico_thread_count;
static	int						pico_desired_thread_count = 1;
static	const char*				pico_fail_actions[4] = {"Failed", "Reading", "Sending", 0};
static  bool					pico_suicide;
static  bool					pico_at_exit_done;
static  std::atomic_int         pico_sock_open_count;


struct PicoLister {
	uint64_t List;
	PicoLister () {List = pico_list.Map;}
	
	PicoComms* Next () {
		auto M = List;
		if (!M) return 0;
		auto Lowest = M & -M;
		auto ID = pico_log2(Lowest);
		List = M &~ Lowest;
		return pico_list.Items[ID];
	}
};


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
		return {SectionStart+T, H-T}; // 🕷️  🕷️ 🕷️
	}
	
	PicoMessage AskUnused () {
		int T = Tail; int H = Head; int S = Size; int B = S-1;
		int L = H - T;
		if (L >= S) return {};
		T &= B; H &= B;
		if (T > H) // head to size, or head to tail.
			S = T;
		return {SectionStart+H, S-H};
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



struct PicoComms {
	volatile int				Guard;
	PicoConfig					Conf;
	int							ID;
	int							Socket;
	std::atomic_int				InUse;
	unsigned char				Status;
	volatile unsigned char		HalfClosed;
	bool						IsParent;
	bool						DestroyMe;
	PicoTrousers				QueueLocker;
	int							LengthBuff;
	PicoMessage					QueueHead;
	PicoMessage*				QueueTail;
	PicoBuff*					Reading;
	PicoBuff*					Sending;
	volatile int				FinalGuard;
	
	PicoComms (int noise, bool isparent, int Size) { // constructor
		memset(this, 0, sizeof(*this));
		ID = -1; Status = -1; IsParent = isparent;
		Guard = 0xB00BE355;
		FinalGuard = 0xF00DCA4E;
		QueueTail = &QueueHead;
		Conf.Noise = noise;
		Conf.SendTimeOut = 10.0f;

		int B = 31 - __builtin_clz(Size);
		if (B < 10) B = 10;
		B += (1<<B < Size);
		Conf.Bits = B;
		Conf.QueueSize = 1<<(B+3);
	}

	~PicoComms () { // destructor
		if (!guard_ok()) return;
		if  (Socket) { // remove this later
			Say("!!!!! ~Assumption failed~ !!!!!");
			io_close(); // io will close it first.
		}
		
		for (auto M = QueueHead; M.Data; M = pico_next_msg(M))
			free(M.Data);
		if (Sending) Sending->Decr();
		if (Reading) Reading->Decr();
		if (CanSayDebug()) Say("Deleted");
		Guard = 0;
		FinalGuard = 0;
	}

	bool InitThread (int Noise, void* Self, PicoThreadFn fn, const char** Args) {
		if (!pico_list or !alloc_buffs()) return false;
		PicoComms* C = new PicoComms(Noise, false, 1<<Conf.Bits);
		Sending->RefCount++;     Reading->RefCount++;  Socket = -1; 
		C->Sending = Reading; C->Reading = Sending; C->Socket = -1;
		
		add_sub();
		C->add_sub();
		return thread_run(C, fn, Self, Args);
	}
	
	PicoComms* InitPair (int Noise) {
		if (!pico_list) return nullptr;
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return nullptr;
		PicoComms* Rz = new PicoComms(Noise, false, 1<<Conf.Bits);
		add_conn(Socks[0]);
		Rz->add_conn(Socks[1]);
		return Rz;
	}

	bool InitSocket (int Sock) {
		IsParent = false;
		return (Sock > 0 or failed(EBADF)) and add_conn(Sock);
	}

	bool InitExec () {
		return InitSocket(FindSock()); 
	}
	
	int FindSock () {
		const char* x = getenv("__PicoSock__");
		if (!x) return -1;
		unsetenv("__PicoSock__");
		return std::atoi(x);
	}
	
	pid_t StealSock (int Succ, pid_t pid) {
		char Data[8];
		*std::to_chars(Data, Data+sizeof(Data), Succ).ptr = 0;
		setenv("__PicoSock__", Data, 1);
		return pid;
	}

	pid_t InitFork (bool WillExec) {
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return -1;
		pid_t pid = fork();
		if (pid < 0) return -1;

		IsParent = pid!=0; // 🕷️_🕷️
		close(Socks[!IsParent]);
		pico_sock_open_count--;
		int S = Socks[IsParent];
		if (!IsParent) {
			pico_thread_count = 0; // Unix doesn't let us keep threads.
			if (WillExec)
				return StealSock(S, pid);
		} 

		add_conn(S);
		return pid; 
	}
	
	bool CanGet () {
		return !(HalfClosed & 1) or QueueHead.Length;
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
			return (!Conf.SendFailCount++) and SayEvent("CantSend: BufferFull");
		
		PicoDate Final = PicoGetDate() + (PicoDate)(Conf.SendTimeOut*65536.0f);
		while (PicoGetDate() < Final) {
			if (HalfClosed&2) return false; // closed!
			sched_yield();
			if (queue_sub(msg, n)) return true;
		}
		
		return (!Conf.SendFailCount++) and SayEvent("CantSend: TimedOut");
	}
	
	PicoMessage Get (float T = 0.0) {
		if (!can_get(T)) return {};

		PicoMessage M = QueueHead;
		QueueLocker.lock();
		PicoMessage H = pico_next_msg(M);
		QueueHead = H;
		if (!H)
			QueueTail = &QueueHead;
		QueueLocker.unlock();
		Conf.QueueSize  +=  M.Length + sizeof(PicoMessage)*2;
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
	
	void AskClose (const char* Why) {
		if (HalfClosed>=3) return;
		if (!Status) Status = ENOTCONN;
		if (CanSayDebug()) Say("AskClose", Why);
		HalfClosed = 255;
	}
	
	void Destroy (const char* Why) {
		if (guard_ok()) {
			AskClose(Why);
			DestroyMe = true;
		}
	}



//// INTERNALS ////
	
	bool guard_ok () {
		if (Guard == 0xB00BE355 and FinalGuard==0xF00DCA4E) return true;
		printf("!!!!Pico: Double-Freed!!!!\n"); // very bad
		return false;
	}

	struct PicoThreadData {
		PicoComms* C; PicoThreadFn fn; void* Self; const char** Args;
	};
	
	static void* pico_thread_wrapper (void* Args) {
		PicoThreadData* D = (PicoThreadData*)Args;
		(D->fn)(D->C, D->Self, D->Args);
		(D->C)->Destroy("ThreadCompleted");
		free(D);
		return 0;
	}
	
	static bool thread_run (PicoComms* C, PicoThreadFn fn, void* Self, const char** Args) {
		pthread_t T = 0;   ;;;/*_*/;;;   // creeping upwards!!
		auto D = (PicoThreadData*)malloc(sizeof(PicoThreadData));
		D->C = C; D->fn = fn; D->Self = Self; D->Args = Args;
		if (!pthread_create(&T, nullptr, pico_thread_wrapper, D) and !pthread_detach(T))
			return true;
		C->Destroy("ThreadFailed");
		free(D);
		return false;
	}

	bool queue_sub (const char* msg, int n) {
		if (!Sending or !Sending->AppendMsg(msg, n)) return false;
		if (Socket == -1) Conf.LastSend = PicoDate();
		return true;
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
	
	bool can_send () {
		int Remain = Sending->Length();
		return !(HalfClosed&2) and Remain and !Status and Socket > 0;
	}
	
	void do_sending () { 
		if (!can_send() or !Sending->WorkerThread.enter()) return;
		
		while ( auto Msg = Sending->AskUsed() ) { // send(MSG_DONTWAIT) does nothing on OSX. but we set non-blocking anyhow.
			int Amount = (int)send(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
  			if (Amount > 0) {
				Sending->lost(Amount);
				Conf.LastSend = PicoGetDate();
				if (CanSayDebug()) Say("|send|", "", Amount);
			} else if (!io_pass(Amount, 2))
				break;
		}
		Sending->WorkerThread.unlock();
	}
	
	bool can_get (float T) {
		if (QueueHead.Length) return true;
		if (!T) return false;
		if (T < 0) T = Conf.SendTimeOut;
		T = std::min(T, 543210000.0f); // 17 years?
		PicoDate Final = PicoGetDate() + (PicoDate)(T*65536.0f);
		timespec ts = {0, 1000000}; int n = T*16000;
		for ( int i = 0;  i < n and !(HalfClosed&1);   i++) {
			nanosleep(&ts, 0);
			if (QueueHead.Length) return true; 
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
		if (Conf.QueueSize < QS)
			return (!Conf.ReadFailCount++) and SayEvent("CantRead: BufferFull");
		
		if (char* Data = (char*)malloc(L+sizeof(PicoMessage)); Data) {
			Reading->Get(Data, L);
			auto T = (PicoMessage*)(Data + L);
			*T = {};
			LengthBuff = 0;
			Conf.QueueSize -= QS;
			QueueLocker.lock();
			*QueueTail = {Data, L};
			QueueTail = T;
			QueueLocker.unlock();
			Conf.LastRead = PicoGetDate();
			return true;
		}
		return failed(ENOBUFS);
	}
	
	bool get_pair_of (int* Socks) {
		int i = pico_sock_open_count;
		if (i > 60) {
//			raise(SIGINT);			// what?
			return false; 			// seems fair?
		}

		if (socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks)) return failed();
		pico_sock_open_count+=2;
		struct linger so_linger = {1, 5};
		for (int i = 0; i < 2; i++)
			if (setsockopt(Socks[i], SOL_SOCKET, SO_LINGER, &so_linger, sizeof so_linger)) return failed();
		return true;
	}
	
	bool alloc_buffs () {
		if (!Sending)
			Sending = PicoBuff::New(Conf.Bits, "Send", this);
		if (!Reading)
			Reading = PicoBuff::New(Conf.Bits, "Read", this);

		if (Reading and Sending and PicoStart(0)) return true;
		
		return failed(ENOBUFS);
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
		Status = 0;
		ID = pico_list.Add(this);
		if (CanSayDebug()) Say("Started");
		return true;
	}  ;;;/*_*/;;;   // creeping upwards!!

	void report_closed_buffers () {
		if (CanSayDebug() ) {
			int SL = Sending->Length();
			int RL = Reading->Length();
			if (SL) Say("Sending", "Still Contains", SL);
			Say("Sent", "", Sending->Tail);
			if (RL) Say("Reading", "Still Contains", RL);
			Say("Read", "", Reading->Head);
		}
	}

	void* failed (int err=errno, int Action=0) {
		if (!Status or Status == 255) {
			if (err == EPIPE and HalfClosed) {
				err = ENOTCONN;
				SayEvent("ClosedGracefully");
			} else {
				SayEvent(pico_fail_actions[Action], strerror(err));
				if (err == EBADF)
					SayEvent("Socket", "", Socket);
			}
		}
		Status = err;
		HalfClosed = 3;
		return nullptr;
	}
		
	bool io_pass (int Amount, int Half) {
		int H = HalfClosed;
		if (!Amount) {
			H |= Half;
			HalfClosed = H;
			return (H >= 3) and failed(EPIPE, Half);
		}
		int e = errno;
		if (e == EAGAIN) return false;
		if (e == EINTR)  return true;
		return failed(e, Half);
	}
	
	bool io_close() {
		int S = Socket; if (!S) return false;
		if (!Reading->WorkerThread.enter()) return false;

		if (S > 0) {
			close(S);
			pico_sock_open_count--;
		}
		report_closed_buffers();
		Socket = 0; 
		if (CanSayDebug()) Say("Closing");
		Reading->WorkerThread.unlock();
		return true;
	}

	void cleanup () {
		if (!Socket and !InUse and DestroyMe) {
			if (CanSayDebug()) Say("Bye");
			int I = ID;
			delete this;
			pico_list.Remove(I);
		}
	}
	
	void io () {
		if (!Socket or !guard_ok()) return;
		InUse++;
		do_reading();
		do_sending();
		if (HalfClosed>=3) io_close();
		InUse--;
	}
};


static void pico_cleanup () {
	PicoLister Items;
	while (auto M = Items.Next())
		M->cleanup();
}


static void pico_work_comms () {
	PicoLister Items;
	while (auto M = Items.Next())
		M->io();

	if (pico_suicide and getppid() <= 1) {
		pico_suicide = false;
		kill(0, SIGQUIT);
	}
	
	float S = (PicoGetDate() - pico_last_activity) * (0.000015258789f * 0.005f);
	pico_sleep(std::clamp(S*S, 0.001f, 0.5f));
}


static int pico_any_still_sending () {
	PicoLister L;
	int Count = 0;
	while (auto M = L.Next())
		Count += M->StillSending();
	return Count;
}


static float PicoRemainDefault = 5.0;
static void pico_keep_alive () {
	auto Remain = PicoRemainDefault;
	while (pico_any_still_sending() and Remain > 0) {
		pico_sleep(0.001);
		Remain -= 0.001;
	};
}


static void* pico_worker (void* Dummy) {
	if (!pico_at_exit_done) {
		pico_at_exit_done = true;
		atexit(pico_keep_alive);
	}

	int p = pico_thread_count++;
	pico_last_activity = PicoGetDate(); // get to work!
	while (true) {
		pico_work_comms(); pico_work_comms(); pico_work_comms();
		if (!p) 
			pico_cleanup();
	}
}

#endif



/// C-API ///
/// **Initialisation / Destruction** ///
extern "C" PicoComms* PicoCreate ()  _pico_code_ (
/// Creates your message-passer.
	if (!pico_list) return nullptr;
	return new PicoComms(PicoNoiseEvents, true, PicoDefaultInitSize);
)

extern "C" void PicoDestroy (PicoComms** Ref, const char* Why=0) _pico_code_ (
/// Destroys the PicoComms object, and reclaims memory. Also closes the other side.
/// Sets Ref to nullptr, to indicate that you really should not use that object!
/// Accepts *Ref == nullptr
	PicoComms* M = *Ref;
	if (M) M->Destroy(Why);
	*Ref = nullptr;
)

extern "C" PicoComms* PicoStartChild (PicoComms* M) _pico_code_ (
// Creates a new child comm and links them together, using sockets.
	return M->InitPair(PicoNoiseEvents);
)

extern "C" bool PicoCompleteExec (PicoComms* M) _pico_code_ (
/// Call this in your child process, (after completing exec), if you passed true to `PicoStartFork`. 
	return M->InitExec();
)

extern "C" bool PicoStartThread (PicoComms* M, PicoThreadFn fn, void* Obj=0, const char** Args=0) _pico_code_ (
/// Creates a new thread, using the function "fn", and passes a newly created PicoComms object to your function! Also cleans up the newly created PicoComms when done. Returns `false` if any error occurred. Look at PicoTest.cpp for a good example. :) You can pass two user-defined parameters.
	return M->InitThread(PicoNoiseEvents, Obj, fn, Args);
) 

extern "C" int PicoStartFork (PicoComms* M, bool WillExec=false) _pico_code_ (
/// This will fork your app, and then connect the two apps with PicoMsg. Returns the result of `fork()`. So handle it just the same.
/// Passing true to WillExec, will prepare PicoMsg for a call to any of the `execve()` family. Its kinda complex to explain, so just look at `PicoTest.cpp` for a good example. The child process should call `PicoCompleteExec` to start up the connection.

	return M?M->InitFork(WillExec):fork();
)


/// **Communications** ///
extern "C" bool PicoSend (PicoComms* M, PicoMessage Msg, int Policy=PicoSendGiveUp) _pico_code_ (
/// Sends the message. The data is copied to internal buffers so you do not need to hold onto it after send. If `CanWait` is false and there is no buffer space, this function returns `false`. If `CanWait` is true, it will block until the timeout is reached. See the ["configuration"](#Configuration) section about how to change the timeout.
	return M->QueueSend(Msg.Data, Msg.Length, Policy);
)

extern "C" bool PicoSendStr (PicoComms* M, const char* Msg, bool Policy=PicoSendGiveUp) _pico_code_ (
/// Same as `PicoSend`, just a little simpler to use, if you have a c-string.
	return M->QueueSend(Msg, (int)strlen(Msg)+1, Policy);
)

extern "C" PicoMessage PicoGet (PicoComms* M, float Time=0) _pico_code_ (
/// Gets a message if any exist. You can either return immediately if none are queued up, or wait for one to arrive.
/// Once it returns a PicoMessage, you must `free()` it's `Data` property, after you are finished with it.
	if (M) return M->Get(Time); return {};
)


/// **Utilities** ///

extern "C" void PicoClose (PicoComms* M, const char* Why) _pico_code_ (
/// Closes the comms object. Does not destroy it. Useful if you have many places that might need to close the comms, but only one place that will destroy it. It acceptable to close a comms twice!
	if (M) M->AskClose(Why);
)

extern "C" void* PicoSay (PicoComms* M, const char* A, const char* B="", int Iter=0) _pico_code_ (
/// Prints a string to `stdout`. This can be used to debug or report things. This helpfully mentions if we are the parent or not, and also mentions our Comm's name. (`Name` is settable via `PicoConfig`).
	return M->Say(A, B, Iter);
)

extern "C" int PicoError (PicoComms* M) _pico_code_ (
/// Returns an error that forced comms to close. If the comms is still open, the error is 0.
	return M?M->Status:-1;
)

extern "C" PicoConfig* PicoConf (PicoComms* M) _pico_code_ (
/// Gets the config struct. You can configure "noise", "timeout", "name", and the maximum unread-message queue size.

	return M?&M->Conf:0;
)

extern "C" bool PicoStillSending (PicoComms* M) _pico_code_ (
///  Returns if the comms object is still in the business of sending. This is to let you keep your app open while busy sending. Pass nil to see if any are sending.
	return M?M->StillSending():pico_any_still_sending();
)

extern "C" void PicoSleepForSend (float During, float After) _pico_code_ (
///  Sleeps while sends occur. Useful to call last in your app.
	PicoRemainDefault = During;
	pico_keep_alive();
	pico_sleep(After);
)

extern "C" bool PicoCanGet (PicoComms* M) _pico_code_ (
/// Returns if we either HAVE unread messages, or MIGHT get them in the future (That is, it is not closed)
	return M?M->CanGet():false;
)

extern "C" bool PicoIsParent (PicoComms* M) _pico_code_ (
/// Returns if we are the parent, or child end of the comms.
	return M->IsParent;
)

extern "C" bool PicoHasParentSocket () _pico_code_ (
/// Returns if our app, created via `PicoStartFork(M, true)` (with the true param). This is in case your app could be launched in multiple ways. Like my compiler speedie can be run normally from the terminal, or run as a sub-process by the IDE, and then speedie will want to know if it was run as a sub-process via the IDE! This function informs speedie of that fact.
	return getenv("__PicoSock__");
)

extern "C" void PicoDesiredThreadCount (int C) _pico_code_ (
// Allows to choose the desired amount of threads.
	pico_desired_thread_count = C;
)

extern "C" bool PicoStart (int Suicide) _pico_code_ (
/// Starts the PicoMsg worker threads. The number of threads created is set via PicoDesiredThreadCount. You can pass 1 to `Suicide` param, to make the worker threads kill the app if it's parent dies. Or pass -1 to set it to false even if it was previously set. Passing 0 will leave it unchanged (defaults to not suiciding).

	if (Suicide)
		pico_suicide = Suicide > 0 and getppid() > 1;
	if (Suicide > 1 or Suicide < -1) return true; // set suicide-only.
	
	pthread_t T = 0;   ;;;/*_*/;;;   // creeping downwards!!
	for (int i = pico_thread_count; i < pico_desired_thread_count; i++)
		if (pthread_create(&T, nullptr, (void*(*)(void*))pico_worker, nullptr) or pthread_detach(T))
			return false;

	return true;
)    ;;;/*_*/;;;  ;;;/*_*/;;;     ;;;/*_*/;;;   // the final spiders

#endif

