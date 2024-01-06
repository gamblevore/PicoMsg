
// picomsg
// Licence: https://creativecommons.org/licenses/by/4.0/

// Simple parent-child queue-based message-passing.
// uses two threads, and is non-blocking


#ifndef __PICO_MSG__
#define __PICO_MSG__

#define PicoSilent				0
#define PicoNoiseDebugChild		1
#define PicoNoiseDebugParent	2
#define PicoNoiseDebug			3
#define PicoNoiseEventsChild	4
#define PicoNoiseEventsParent	8
#define PicoNoiseEvents			12

struct			PicoComms;
struct			PicoMessage        { char* Data;       int Length;  operator bool () {return Data;} };
struct 			PicoMessageConfig  { const char* Name; int LargestMsg; int Noise; int TotalReceived; int TotalSent; };

#ifndef PICO_IMPLEMENTATION
	#define _pico_code_(x) ;
#else
	#define _pico_code_(x) {x}

	#include <poll.h>
	#include <fcntl.h>
	#include <stdio.h>
	#include <unistd.h>
	#include <string>
	#include <errno.h>
	#include <sys/socket.h>
	#include <csignal>
	#include <atomic>
	#include <deque>
	#include <algorithm>


using std::deque;
using std::atomic_bool; using std::atomic_int; using std::atomic_uint64_t;
struct PicoTrousersLocker {
	struct PicoTrousers& Lock; 
	~PicoTrousersLocker ();
};

struct PicoTrousers { // only one person can wear them at a time.
	atomic_bool Value;
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
	PicoTrousersLocker lock () {
		while (!enter()); // spin
		return {*this};
	}
};

PicoTrousersLocker::~PicoTrousersLocker () {Lock.unlock();}


struct PicoRange {
	uint64_t Range;	
	PicoRange (uint64_t R = 0) {Range = R;}
	int Length () {return Range&0xFFFFffff;}
	int Start () {return Range>>32;}
	void Set (uint64_t Start, uint64_t Length) {Range = (Start<<32)|Length;}
};


typedef int64_t			PicoDate;  // 16 bits for small stuff
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
	atomic_int				RefCount;
    PicoCommsBase*			Next;
    PicoCommsBase*			Prev;
	PicoMessageConfig		Conf;
	
	PicoCommsBase () { Prev = this; Next = 0;}
	
	void AddComm (PicoCommsBase* New) {
		auto L = PicoCommsLocker.lock();
		PicoCommsBase* N = Next;
		if (N)
			N->Prev = New;
		New->Next = N;
		New->Prev = this;
		Next = New;
	}
	
	;;;/*_*/;;;   // <-- a spider that ehlps you do async work
	void RemoveComm () {
		auto L = PicoCommsLocker.lock();
		PicoCommsBase* N = Next;
		PicoCommsBase* P = Prev;
		if (N)
			N->Prev = P;
		P->Next = N;
		Next = 0;
		Prev = 0;
	}
	
	PicoComms* NextComm () {
		auto L = PicoCommsLocker.lock();
		auto N = Next;
		if (N)
			N->RefCount++;
		return (PicoComms*)N;
	}
};


static	PicoCommsBase	PicoList;
static	PicoDate		PicoLastActivity;
static	void*			pico_worker (void* obj);
static	atomic_int		PicoThreadCount;

bool pico_start () {
	for (int i = PicoThreadCount; i < 1; i++) {
		pthread_t T = 0;
		if (pthread_create(&T, 0, pico_worker, 0) or pthread_detach(T))
			return puts("Pico: Unable to create threads.")*false;
	}
	return true;
}    ;;;/*_*/;;;  ;;;/*_*/;;;     ;;;/*_*/;;;   // more spiders





struct PicoBuff { // We have to move this code DOWN to get away from the spiders! 😠
	const char*			Name;
	char*				SectionStart;
	atomic_uint64_t		Range;
	int					Size;
	PicoTrousers		IO;
	
	PicoBuff () {SectionStart = 0; Clear();}
	~PicoBuff () {Clear();}
	
	bool Alloc (int n, const char* name) {
		Name = name;
		if (!(SectionStart = (char*)calloc(n, 1))) return false;
		Size = n;
		return true;
	}
	
	void Clear () {
		free(SectionStart); SectionStart = 0; Size = 0; Range = 0; Name = "";
	}
	
/*
	||||||||------- // start
	----||||------- // middle
	------||||||||| // end (partial)
	||||||||||||||| // end (whole)
	|||||-----||||| // overlapping
*/
	
	PicoMessage AskUsed () {
		auto R = PicoRange(Range); 
		int L = R.Length();
		if (!L) return {};

		int S = R.Start();
		auto From = SectionStart+S;
		return {From, std::min(L, Size-S)};
	}

	void Lost (int N) {
		PicoLastActivity = PicoGetDate();
		auto Lck = IO.lock();	
		auto R = PicoRange(Range); 
		int L = R.Length() - N;
		int S = R.Start() + N;
		if (S >= Size)
			S = 0;
		R.Set(S, L);
		Range = R.Range;
	}
	
	int Length () { return PicoRange(Range).Length(); }; 

	void Gained (int N) { 
		PicoLastActivity = PicoGetDate();
		auto Lck = IO.lock();	
		auto R = PicoRange(Range); 
		R.Set(R.Start(), R.Length() + N);
		Range = R.Range;
	}

	bool Append (const char* Src, int ML) {
		int L = htonl(ML);
		return AppendSub((char*)&L, 4) and AppendSub(Src, ML);
	}
	
	bool AppendSub (const char* Src, int Need) {
		while (auto Dest = AskUnused()) {
			int Avail = std::min(Need, Dest.Length);
			Need -= Avail;
			Gained(Avail);
			memcpy(Dest.Data, Src, Avail);
			if (Need <= 0) return true;
		};
		return false;
	}
	
	PicoMessage AskUnused () {
		auto R = PicoRange(Range); 
		auto L = R.Length();
		auto M = Size; 
		if (L >= M) return {};
		auto S = R.Start();
		auto FromPos = L + S;
		if (FromPos >= M) {
			FromPos -= M;
			M = S;
		}
		
		return {SectionStart + FromPos, M - FromPos};
	}
	
	bool Get (char* Dest, int N) {
		while (auto Msg = AskUsed()) {
			int B = std::min(Msg.Length, N);
			memcpy(Dest, Msg.Data, B);
			Dest += B;
			N -= B;
			Lost(B);
			if (N <= 0) break;
		}
		return true;
	}
}; 



static char PicoCloseData[4] = {};

struct PicoComms : PicoCommsBase {
	int					Socket;
	unsigned char		Err;
	bool				IsParent;
	bool				WantClose;
	int					LengthBuff;
	PicoBuff			Reading;
	PicoBuff			Sending;
	
	PicoComms (int noise, bool isparent) {
		WantClose = false; RefCount = 1; Socket = 0; Err = 0; IsParent = isparent;
		Conf.Name = isparent?"Parent":"Child";
		Conf.Noise = noise;
		if (!Sending.Alloc(1024*1024, "send") or !Reading.Alloc(1024*1024, "read"))
			failed(ENOBUFS);
	}
	~PicoComms () { 
		if (Socket)
			Socket = close(Socket)&0;
		SayEvent("deleted");
	}

//
	PicoComms* Pair (int Noise) {
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return 0;
		PicoComms* Rz = new PicoComms(Noise, false);
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
		Conf.Name = IsParent?"Parent":"Child";
		close(Socks[!IsParent]);
		add_conn(Socks[IsParent]);

		if (!pico_start()) {
			disconnect("Thread Failed");
			if (!IsParent) exit(-1);
		}
		
		if (!IsParent)
			PicoThreadCount = 0; // Unix doesn't let us keep threads.
		return pid; 
	}
	
	bool QueueSend (const char* msg, int n) {
		if (WantClose) return false;
		if (!Sending.Append(msg, n)) return failed(ENOBUFS);
		SayDebug( "QueuedForSend", "", n);
		return true;
	}

	PicoMessage Get (float T = 0.0) {
		PicoMessage M = get_sub();
		if (M or T <= 0) return M;
		PicoDate Final = PicoGetDate() + (PicoDate)(T*65536.0f);
		timespec ts = {0, 1000000}; int n = T*16000;
		for ( int i = 0;  Socket and !M and i < n;   i++) {
			nanosleep(&ts, 0);
			if (PicoGetDate() > Final) break;
			M = get_sub();
		}
		return M;
	}

	void* SayEvent (const char* A, const char* B="", int Iter=0) { return Say(A, B, Iter, 2); }

	void* SayDebug (const char* A, const char* B="", int Iter=0) { return Say(A, B, Iter, 1); }
	
	void* Say (const char* A, const char* B, int Iter, int Strength) {
		if (Strength < 3) {
			int N = Conf.Noise & ((PicoNoiseDebugChild+PicoNoiseEventsChild) << IsParent);
			if (!(N&3 and Strength > 0) and !(N&12 and Strength == 2))
				return nullptr;
		}
		
		const char* S = IsParent?"Us":"Them";
		
		if (Iter)
			printf("%s.%s says: %s %s %i\n", S, Conf.Name, A, B, Iter);
		  else
			printf("%s.%s says: %s %s\n", S, Conf.Name, A, B);
		
		return nullptr;
	}
	


//// INTERNALS ////
	
	void do_reading () {
		while ( auto Msg = Reading.AskUnused() ) {
			int Amount = (int)recv(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
			if (Amount > 0) {
				Reading.Gained(Amount);
				pico_sleep(0.001);
//				Say("Read", "", Amount);
			} else if (!io_pass(Amount))
				break;
		}
	}
	
	void do_sending () {
		if (Err)
			return;
		
		// needs a lock to stop multiple threads sending at the same time!
		while ( auto Msg = Sending.AskUsed() ) {
			int n = std::min(Msg.Length, 1*1024); // can stall otherwise
			int Amount = (int)send(Socket, Msg.Data, n, MSG_NOSIGNAL|MSG_DONTWAIT);
  			if (Amount > 0) {
				Sending.Lost(Amount);
			} else if (!io_pass(Amount))
				break;
		}
	}
	
	PicoMessage nothing (void* nuffing) {return {};}

	PicoMessage get_sub () {
		int L = LengthBuff; 
		if (!L) {
			int Pend = Reading.Length();
			if (Pend < 4) return {};
			Reading.Get((char*)&LengthBuff, 4);
			L = LengthBuff = ntohl(LengthBuff); 
		}
			
		if (L <= 0)
			return nothing(failed(EILSEQ));

		if (Reading.Length() < L) return {};
		char* Data = (char*)malloc(L);
		if (Data) {
			Reading.Get(Data, L);
			LengthBuff = 0;
			return {Data, L};
		}
		return nothing(failed(ENOBUFS));
	}
	
	bool get_pair_of (int* Socks) {
		return  socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks)==0  or  failed();
	}
	
	void add_conn (int Sock) {
		Socket = Sock;
		PicoList.AddComm(this);
		SayEvent("Started");
	}
	
	void* disconnect (const char* Why) {
		if (WantClose) return 0;
		if (!Err) Err = ENOTCONN; // this blocks sending! but not reading.
		WantClose = true;
		return SayEvent("Disconnect", Why);
	}

	void destroy () {
		disconnect(!Err?"Successful":"");
		RemoveComm();
		decr();
	}

	void decr() {
		if (!--RefCount)
			delete this;
	}

	void* failed (int err=errno) {
		Err = err;
		return SayEvent("Failed", strerror(err));
	}
		
	bool io_pass (int Amount) {
		if (!Amount) return false;
		int e = errno;
		if (e == EAGAIN) return false;
		if (e == EINTR)  return true;
		return failed(e);
	}
	
	PicoComms* io () {
		if (Socket) {
			do_reading();
			do_sending();
			if (WantClose) {
				Socket = close(Socket)&0;
				Sending.Clear(); // reading still may have messages?
			}
		}
		auto N = NextComm();
		decr(); 
		return N;
	}
};



static void pico_work_comms () {
	auto M = PicoList.NextComm();
	while (M)
		M = M->io();
	
	float S = 1000.0f;
	if (PicoLastActivity)
		S = (PicoGetDate() - PicoLastActivity) * 0.000015258789f; // seconds
	S = std::clamp(S, 0.03f, 0.9999f);
	pico_sleep(S*S);
}


static void* pico_worker (void* T) {
	PicoThreadCount++;
	while (true) pico_work_comms();
	PicoThreadCount--; // nice
}

#endif



/// C-API ///
extern "C" PicoComms* PicoMsgComms (int Noise=PicoNoiseEvents)  _pico_code_ (
	return new PicoComms(Noise, true);
)

extern "C" PicoComms* PicoMsgCommsPair (PicoComms* M, int Noise=PicoNoiseEvents) _pico_code_ (
	return M->Pair(Noise);
)

extern "C" void PicoMsgDestroy (PicoComms* M) _pico_code_ (
	if (M) M->destroy();
)

extern "C" int PicoMsgFork (PicoComms* M) _pico_code_ (
	return M->Fork();
)

extern "C" int PicoMsgErr (PicoComms* M) _pico_code_ (
	return M->Err;
)

extern "C" void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0) _pico_code_ (
	return M->Say(A, B, Iter, 3);
)

extern "C" bool PicoMsgSend (PicoComms* M, PicoMessage Msg) _pico_code_ (
	if  (!Msg.Data  or  Msg.Length <= 0) return false;
	return M->QueueSend(Msg.Data, Msg.Length);
)

extern "C" bool PicoMsgSendStr (PicoComms* M, const char* Msg) _pico_code_ (
	return M->QueueSend(Msg, (int)strlen(Msg)+1);
)

extern "C" PicoMessage PicoMsgGet (PicoComms* M, float Time=0) _pico_code_ (
	return M->Get(Time);
)

extern "C" PicoMessageConfig* PicoMsgConf (PicoComms* M) _pico_code_ (
	return &M->Conf;
)

#endif
