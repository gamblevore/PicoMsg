
#define PICO_IMPLEMENTATION
// #define PICO_LOG "/tmp/PicoTest"

#include "PicoMsg.h"
#include <vector>
using std::vector;
#include <iostream>
#include <bitset>


extern char **environ;
std::atomic_int FinishedBash;
const char* SelfPath;
bool AllSleep;


static void* BashReserve (int T) {
	int Remain = 5000;
 	int P = 0;
	
	while (Remain > 0) {
		if (!P) {
			P = pico_list.Reserve();
			continue;
		}
		auto P2 = pico_list.Reserve();
		if (P2) {
			Remain--;
			timespec ts = {0, 100*(T+1)}; nanosleep(&ts, 0);
			pico_list.Remove(P2-1);
			pico_list.Remove(P-1);
			P = 0;
			P2 = 0;
			if ((Remain % 32 == 0) or AllSleep) {
				puts("Sleeping...");
				sleep(5);
			}
		}
	}
	FinishedBash++;
	return 0;
}


static void* BashCreation (int T) {
	int Remain = 5000;
 	PicoComms* P = 0;
	
	while (Remain > 0) {
		if (!P) {
			P = PicoCreate("Basher2");
			continue;
		}
		P->Noise = 0;
		auto P2 = PicoStartChild(P);
		if (P2) {
			strcpy(P2->Name, "Second");
			Remain--;
			P2->Noise = 0;
			if ((Remain & 8) == 0) {
				timespec ts = {0, 100*(T+1)}; nanosleep(&ts, 0);
			}
			P = PicoDestroy(P);
			P2 = PicoDestroy(P2);
			if ((Remain % 32 == 0) or AllSleep) {
				puts("Sleeping...");
				sleep(1);
			}
		}
	}
	FinishedBash++;
	return 0;
}

uint hash (uint x) {
	x ^= x >> 16;
	x *= UINT32_C(0x7feb352d);
	x ^= x >> 15;
	x *= UINT32_C(0x846ca68b);
	x ^= x >> 16;
	return x;
}


void* ThreadQuery (void* TM) {
	PicoComms* M = (PicoComms*)TM;
	strcpy(M->Name, "Query");
	PicoSendStr(M, "mary had a little lamb");
	const int Stack = 49; const int Pudge = 4096;
	vector<char> abcd(Stack*Pudge);
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] = (char)(hash(i)%26)+'A';
	}
	
	vector<PicoMessage> Sent;
	vector<PicoMessage> Got;
	char* abc = &abcd[0];
	int Remain = Stack*Pudge;
	while (Remain > 0) {
		if (PicoError(M)) return PicoSay(M, "Exit: CantSendAll");

		int n = hash(Remain) % Pudge;
		if (n > Remain) n = Remain;
		if (n < 1) n = 1;
		Remain -= n;
		Sent.push_back({abc, n});
		PicoSend(M, abc, n, PicoSendCanTimeOut);
		auto Back = PicoGetCpp(M);
		if (Back) {
			PicoSay(M, "User Got:", "", Back.Length);
			Got.push_back(Back);
		}
		abc += n;
	}
	
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] ++;
	}

	PicoSay(M, "Comparing", "", (int)Sent.size());
	
	while (auto Back = PicoGetCpp(M, 7*(Sent.size() > Got.size()))) {
		Got.push_back(Back);
	}

	if (int Diff = (int)(Sent.size() - Got.size()); Diff) 
		return PicoSay(M, "Missing Responses: ", "", Diff);

	for (int i = 0; i < Sent.size(); i++) {
		auto& v = Sent[i];
		auto& OK = Got[i];
		if (OK.Length != v.Length)				return PicoSay(M, "Exit: BadLength");
		int diff = memcmp(v.Data, OK.Data, v.Length);
		if (diff)								return PicoSay(M, "Exit: BadContents");
		abc += v.Length;
		PicoSay(M, "Passed", "", i+1);
		free(OK.Data);
	}
	PicoClose(M, "Exit: Tests Passed!");
	return 0;
}


bool GetAndSay (PicoComms* M, float t) {
	auto Mary = PicoGetCpp(M, fabs(t));
	if (Mary.Data) {
		PicoSay(M, "Got", Mary.Data);
		free(Mary.Data);
		return true;
	}
	if (t > 0)
		PicoSay(M, "failed get");
	return false;
}


void ThreadRespond (PicoComms* M, uint Mode, const char** Args) {
	strcpy(M->Name, "ThreadRespond");
	GetAndSay(M, 6.0);
	
	int n = 0;
	while (!PicoError(M)) {
		auto Msg = PicoGetCpp(M, 1.3);
		if (!Msg) {
			PicoSay(M, "NoMoreInput", "", n);
			break;
		}

		for (int i = 0; i < Msg.Length; i++)
			(Msg.Data)[i]++;

		PicoSend(M, Msg.Data, Msg.Length, PicoSendCanTimeOut);
		free(Msg.Data);
		n++;
	}
	
	PicoSay(M, "Responses Given:", "", n);
}


int TestPair (PicoComms* C) {
	auto C2 = PicoStartChild(C);
//	C2->Noise = -1;
//	 C->Noise = -1;
	
	if (C2) {
		PicoSendStr(C, "pear🍐🍐🍐test");
		PicoSendStr(C, "🏀🏀🏀");
		while (GetAndSay(C2, -0.1)) {;}
		PicoSendStr(C, "🏀🏀🏀");
		while (GetAndSay(C2, -0.1)) {;}
		PicoSendStr(C, "🏀🏀🏀");
		PicoSendStr(C, "🏀🏀🏀");
		PicoSendStr(C, "🧟‍♂️ 👀 👀 👀 👀");
		while (GetAndSay(C2, -0.1)) {;}
	}
	PicoDestroy(C2, "Finished");
	return 0;	
}


int TestWrite (char* Out, int i, char Base=0) {
	int j = 0;
	// write text num, and text
	for (int reps = 0; reps <= i % 3; reps++) {
		int x = i;
		while (j < 16) {
			int y = x % 26;
			x /= 26;
			Out[j++] = y + 'A' + Base;
			if (x == 0)
				break;
		}
	}
	Out[j] = 0;
	return j+1;
}


int RecIndex = 0;
bool TestIntenseCompare (PicoComms* C, float T) {
	char Expected[20] = {};
	auto Rec = PicoGetCpp(C, T); if (!Rec) return false;
	auto Found = Rec.Data;
	TestWrite(Expected, RecIndex);
	printf("<-- %s %i\n", Found, RecIndex);
	bool Rz = strcmp(Expected, Found) == 0;
	if (!Rz) {
		printf("String %i differed! Expected '%s' but found '%s'\n", RecIndex, Expected, Found);
		T = T;
	}
	RecIndex++;
	free(Found);
	return Rz;
}


int TestFork (PicoComms* C) {
	strcpy(C->Name, "Tester");
	int PID = PicoStartFork(C, "Fixer");
//	C->Noise = PicoNoiseAll;
	if (PID < 0)
		return -PID;
	
	if (PID) {
		char Out[20] = {}; memset(Out, -1, sizeof(Out));
		PicoMessage Snd = {Out, 0};
		PicoSay(C, "Asks intensely");
		int MaxTests = 100000;
		for (int i = 0; i < MaxTests; i++) {
			Snd.Length = TestWrite(Out, i, 1);
			if (!PicoSend(C, Snd.Data, Snd.Length))
				return !PicoSay(C, "Exitting Sadly");
			PicoSay(C, "Sending", Snd.Data, i);
			while (TestIntenseCompare(C, 0)) {;}
		}
		PicoSay(C, "AllSent!");
		while (RecIndex < MaxTests and TestIntenseCompare(C, 10)) {;}
		PicoSay(C, "strings compared", "", RecIndex);
		PicoClose(C, "Completed");

	} else {
		int Back = 0;
		char Out[20] = {}; memset(Out,-1,sizeof(Out));
		while (auto Msg = PicoGetCpp(C, 10.0)) {
			auto D = Msg.Data; Msg.Data = Out;
			int n = Msg.Length-1;
			for (int j = 0; j < n; j++)
				Out[j] = D[j] - 1;
			Out[n] = 0;

			if (!PicoSend(C, Msg.Data, Msg.Length))
				return !PicoSay(C, "Exitting Sadly");
			free(D);
			Back++;
			if (PicoError(C)) break;
		}
		PicoSay(C, "NoMoreInput", "", Back);
	}
	
	PicoSay(C, "Acheived"); return 0;	
}

int TestThread (PicoComms* C) {
	if (!PicoStartThread(C, ThreadRespond)) return -1;
	ThreadQuery(C);
	PicoSleep(1.0); // let ThreadRespond exit
	return 0;	
}


int ThreadBash (PicoComms* B, void* Fn) {
	// Threadedly create/destroy a load of PicoComm*'s
	const int ThreadCount = 4;

	pthread_t T = 0;
	for (long i = 0; i < ThreadCount; i++)
		if (pthread_create(&T, nullptr, (void*(*)(void*))Fn, (void*)i) or pthread_detach(T))
			return false;
	
	
	for (int i = 1; true; i++) {
		AllSleep = (i&32)==0;
		bool WillExit = FinishedBash >= ThreadCount;
		int SockO = pico_open_sockets;
		uint64_t Map = pico_list.Map;
		printf("pico open sockets: %i", SockO);
		std::cout << ",  Map: " << std::bitset<64>(Map) << std::endl;
		if (WillExit) break;
		PicoSleep(0.25);
	}
	
	printf("Bashed %i threads!\n", ThreadCount);
	return 0;
}


bool GetAndSayExec (PicoComms* M, float t, int i) {
	auto Mary = PicoGetCpp(M, fabs(t));
	if (Mary.Data) {
		i++;
		PicoSay(M, "Got", Mary.Data);
		if (strncmp("bcd", Mary.Data, 3)) {
			PicoSay(M, "no bcd at", 0, i);
			return false;
		}
		int x = atoi(Mary.Data+3);
		if (x!=i) {
			PicoSay(M, "mismatch at", 0, i);
			exit(-1);
			return false;
		}
			
		free(Mary.Data);
		return true;
	}
	if (t > 0 and i!=10000)
		PicoSay(M, "failed get");
	return false;
}


int TestExec (PicoComms* C) {
	strcpy(C->Name, "ExecParent");

	const char* Args[3] = {SelfPath, "exec", 0};
	int PID = PicoExec(C, "Child", Args);
	if (PID < 0) return -PID;

	char Data[20] = {'a', 'b', 'c'};
	int Back = 0;
	PicoMessage M = {Data, 0};
	for (int i = 0; i < 10000; i++) {
		M.Length = C->TextNumber(i+1, Data+3)+4; // send a zero-terminated value.
		if (PicoSend(C, M.Data, M.Length))
			PicoSay(C, "Sent", Data);
		if (GetAndSayExec(C, 0.0, Back))
			Back++;
	}
	PicoSleep(1.0);
	while (GetAndSayExec(C, 2.0, Back))
		Back++;
	
	PicoSay(C, "Total", "", Back);
	return 0;
}


int TestExec2 (PicoComms* C) {
	strcpy(C->Name, "ExecChild");
	if (!PicoRestoreExec(C)) return -1;
	while (auto M = PicoGetCpp(C, 10)) {
		PicoSay(C, "Got", M.Data);
		for (int i = 0; i < M.Length-1; i++) {
			auto ch = M.Data[i];
			if (ch < '0' or ch > '9')
				M.Data[i]++;
		}
		while (!PicoSend(C, M.Data, M.Length, PicoSendCanTimeOut) and PicoError(C) == 0);
		PicoSay(C, "Sent", M.Data);
	}
	return 0;
}


int TestPipeChild () {
	for (int i = 0; i < 1000; i++) {
		PicoSleep(0.001);
		if (!(i%32))
			dprintf(STDERR_FILENO, "%i, ", i-1);
		printf("ABCDEFGH: %i\n", i+1);
	} 
	return 0;
}


int TestWaiting () {
	printf("Starting %i\n", getpid());
	PicoDate TimeOut = PicoNow() + 5*64*1024;
	while (PicoNow() < TimeOut) {
		printf("TimeLeft %i: %.1f\n", getpid(), (float)(TimeOut - PicoNow())/(64*1024.0));
		PicoSleep(1);
	}
	printf("Exiting %i\n", getpid());
	return 0;
}


int TestPipe (PicoComms* C) {
	/// Uses pico to read `stdout` of a subprocess.
	/// Using `read()` on the main thread causes lag. But Pico is threaded, so it doesn't have that issue.
	strcpy(C->Name, "PipeParent");
	const char* Args[3] = {SelfPath, "pipe", 0};
	int PID = PicoShellExec(C, "pipe", Args);
	if (PID < 0) return -1;

	PicoDate TimeOut = PicoNow() + 5*64*1024;
	while (true) {
		auto Piece = PicoStdOut(C);
		if (!Piece) {
			if (PicoNow() < TimeOut)
				continue;
			if (auto Errs = PicoStdErr(C); Errs)
				puts(Errs.Data);
			puts("No more input");
			return 0;
		}
		TimeOut = PicoNow() + 0.5*64*1024;
		puts(Piece.Data);
	}
	return 0;
}


int TheActualChild () {
	PicoDate Now = PicoNow();
	PicoDate Then = Now + ((Now&7)<<16); // up to 7 seconds extra
	int i = 0;
	int p = getpid();
	while (PicoNow() < Then) {
		i++;
		printf("PID %i: %i\n", p, i);
		sleep(1);
	}
	return Now&2;
}


int TestChildren (PicoComms* C) {
	/// Shows how to use pico to manage subprocesses.
	puts("------\nShould say 'No such file or directory' or 'successful'.\n'No such file or directory' just tests we can receive errors.\n------\n");
	strcpy(C->Name, "ProcParent");
	const char* Args[3] = {SelfPath, "children", 0};
	PicoComms* Ch[10] = {};
	
	int Alive = 0;
	for (int i = 0; i < 10; i++) {
		char Name[4] = {'c', 'h', (char)('0' + i), 0};
		Ch[i] = PicoCreate(Name);
		Alive += PicoExec(Ch[i], Name, Args, true) > 0;
	}
	
	while (Alive > 0) {
		sleep(1);
		for (int i = 0; i < 10; i++) {
			C = Ch[i];
			if (C and PicoStatus(C, 0) >= 0) {	// Finished!
				PicoProcStats S; PicoStatus(C, &S);
				printf("Process %i (%s) %s\n", S.PID, C->Name, S.StatusName);
				auto Output = PicoStdOut(C);
				if (Output) puts(Output.Data);
				Ch[i] = 0;
				Alive--;
			}
		}
	}
	
	puts("All processes completed");
	return 0;
}


int TestSleep (PicoComms* C) {
	/// Tests sleep CPU usage
	strcpy(C->Name, "ProcParent");
	const char* Args[3] = {SelfPath, "sleep", 0};
	PicoComms* Ch[10] = {};
	
	int Alive = 0;
	for (int i = 0; i < 10; i++) {
		char Name[4] = {'c', 'h', (char)('0' + i), 0};
		Ch[i] = PicoCreate(Name);
		Alive += PicoExec(Ch[i], Name, Args, true) > 0;
	}
	
	printf("We just test the sleep energy of %i processes\n", Alive);
	PicoDate Next = 0;
	
	for (int Count = 0; true;) {
		if (PicoNow() >= Next) {
			for (int i = 0; i < 10; i++) {
				PicoSendStr(Ch[i], "sleep");
			}
			Next = PicoNow() + 64*1024*10; // send a message every 10 seconds
			printf("Sent 'sleep' to %i processes. Occurance: %i\n", Alive, ++Count);
		}
		sleep(1);
	}
	
	return 0;
}


int TestSleeper () {
	PicoInit(); 
	while (getppid() > 1) {
		sleep(1);
	}
	return 0;
}


int TestPrintAlot () {
	int n = (1024*1024)>>3;
	for (int i = 1; i <= n; i++) {
		printf("%07d\n", i);
	}
	return 0;
}


int TestALot (PicoComms* C) {
	strcpy(C->Name, "ProcParent");
	const char* Args[3] = {SelfPath, "alot", 0};
	char Name[3] = {'c', 'h', 0};
	PicoComms* Ch = PicoCreate(Name);
	PicoExec(Ch, Name, Args, true, 0);
	
	puts("Parent Sleeping");
	PicoSleep(2.0); 						// we want to read all... but only after sleeping.
	while (PicoStatus(Ch) < 0) {
		while (auto Str = PicoStdOut(Ch)) 
			printf("%s", Str.Data);
		PicoSleep(0.01);
	}
	puts("Parent Finished");
	
	return 0;
}



void DetectStatususus (PicoComms** Ch, int n) {
	for (int i = 0; i < n; i++) {
		PicoProcStats S;
		PicoStatus(Ch[i], &S);
		printf("%s is %s\n", Ch[i]->Name, S.StatusName);
	}
}


int TestKill (PicoComms* C) {
	PicoDestroy(C);											// unneeded.
	const char* Args[3] = {SelfPath, "wait", 0};
	PicoComms* Ch[5] = {};

	for (int i = 0; i < 5; i++) {
		char Name[4] = {'c', 'h', (char)('0' + i), 0};
		auto C = PicoCreate(Name);
		Ch[i] = C;
		PicoExec(C, Name, Args, true);
	}

	PicoKill(0);
	DetectStatususus(Ch, 5);
	puts("sleeping 5s");
	PicoSleep(5);
	DetectStatususus(Ch, 5);
	puts("Exiting Parent");
	return 0;
}



#define mode(b) (strcmp(S, #b)==0)
int main (int argc, const char * argv[]) {
	const char* S = argv[1];
	SelfPath = argv[0];
	if (!S) S = "1";
	if mode(pipe)
		return TestPipeChild();	
	if mode(children)
		return TheActualChild();
	if mode(wait)
		return TestWaiting();
	if mode(sleep)
		return TestSleeper();
	if mode(alot)
		return TestPrintAlot();
	
	auto C = PicoCreate(S);
	if mode(exec)
		return TestExec2(C);
	
	puts(SelfPath); // to help me debug this from the terminal. xcode buries builds somewhere.
	printf("%i --> %.1fK\n", (int)sizeof(PicoComms), (float)sizeof(pico_all)/1024.0);
	C->Say("Starting Test: ");
	int rz = 0;
	if mode(0)
		rz = TestThread(C);
	  else if mode(1)
		rz = TestFork(C);
	  else if mode(2)
		rz = TestPair(C);
	  else if mode(3)
		rz = TestExec(C);
	  else if mode(4)
		rz = TestKill(C);
	  else if mode(5)
		rz = TestPipe(C);
	  else if mode(6)
		rz = TestChildren(C);
	  else if mode(7)
		rz = ThreadBash(C, (void*)BashCreation);
	  else if mode(8)
		rz = ThreadBash(C, (void*)BashReserve);
	  else if mode(9)
		rz = TestSleep(C);
	  else if mode(10)
		rz = TestALot(C);
	  else {
		errno = ENOTSUP;
		perror("invalid test mode");
		return -1;
	}
	PicoDestroy(C, "Finished");
	return rz;
}

