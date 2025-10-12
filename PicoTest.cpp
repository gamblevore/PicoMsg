
#define PICO_IMPLEMENTATION
#define PICO_LOG "/tmp/PicoTest"

#include "PicoMsg.h"
#include <vector>
using std::vector;
#include <iostream>
#include <bitset>


std::atomic_int FinishedBash;

static void* BasherThread (int T) {
	int Remain = 1000;
 	PicoComms* P = 0;
	
	while (Remain > 0) {
		if (!P) {
			P = PicoCreate("Basher2");
			continue;
		}
		P->Conf.Noise = 0;
		auto P2 = PicoStartChild(P);
		if (P2) {
			strcpy(P2->Conf.Name, "Second");
			Remain--;
			P2->Conf.Noise = 0;
			timespec ts = {0, 100*(T+1)}; nanosleep(&ts, 0);
			P = PicoDestroy(P);
			P2 = PicoDestroy(P2);
			if (Remain % 256 == 0) {
				puts("Sleeping...");
				sleep(5);
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
	strcpy(M->Conf.Name, "Query");
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


bool ThreadRespond (PicoComms* M) {
	strcpy(M->Conf.Name, "ThreadRespond");
	GetAndSay(M, 6.0);
	
	int n = 0;
	while (!PicoError(M)) {
		auto Msg = PicoGetCpp(M, 1.3);
		if (!Msg) {
			PicoSay(M, "NoMoreInput", "", n);
			break;
		}

		for (int i = 0; i < Msg.Length; i++) {
			(Msg.Data)[i]++;
		}
		PicoSend(M, Msg.Data, Msg.Length, PicoSendCanTimeOut);
		free(Msg.Data);
		n++;
	}
	
	PicoSay(M, "Responses Given:", "", n);
	return true;
}


int TestPair (PicoComms* C) {
	auto C2 = PicoStartChild(C);
//	C2->Conf.Noise = -1;
//	 C->Conf.Noise = -1;
	
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
	strcpy(C->Conf.Name, "Tester");
	int PID = PicoStartFork(C, "Fixer");
	C->Conf.Noise = PicoNoiseAll;
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
	if (!PicoStartThread(C, (PicoThreadFn)ThreadRespond)) return -1;
	ThreadQuery(C);
	pico_sleep(1.0); // let ThreadRespond exit
	return 0;	
}


extern char **environ;

int ThreadBash (PicoComms* B) {
	// Threadedly create/destroy a load of PicoComm*'s
	const int ThreadCount = 4;

	pthread_t T = 0;
	for (long i = 0; i < ThreadCount; i++)
		if (pthread_create(&T, nullptr, (void*(*)(void*))BasherThread, (void*)i) or pthread_detach(T))
			return false;
	
	while (true) {
		bool WillExit = FinishedBash >= ThreadCount;
		int SockO = pico_sock_open_count;
		uint64_t Map = pico_list.Map;
		printf("pico open sockets: %i", SockO);
		std::cout << ",  Map: " << std::bitset<64>(Map) << std::endl;
		if (WillExit) break;
		pico_sleep(0.25);
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

int TestExec (PicoComms* C, const char* self) {
	strcpy(C->Conf.Name, "ExecParent");

	const char* Args[3] = {self, "exec", 0};
	int PID = PicoSimpleExec(C, "Child", Args);
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
	pico_sleep(1.0);
	while (GetAndSayExec(C, 2.0, Back))
		Back++;
	
	PicoSay(C, "Total", "", Back);
	return 0;
}


int TestExec2 (PicoComms* C) {
	strcpy(C->Conf.Name, "ExecChild");
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


int TestPipeChild (int fd) {
	for (int i = 0; i < 1000; i++) {
		pico_sleep(0.001);
		printf("ABCDEFGH: %i\n", i+1);
	} 
	return 0;
}


int TestPipe (PicoComms* C) {
	/// Uses pico to read `stdout` of a subprocess.
	/// Avoids using `read()` directly, which has issues (in my experience)
	/// `read()` suffers unavoidable lag, meaning it single-threaded for real-time situations
	/// This can be adapted to read `stderr`. You would need one PicoComm for each.

	return 0;
}


int main (int argc, const char * argv[]) {
	const char* S = argv[1];
	if (!S) S = "1";
	int rz = 0;
	auto C = PicoCreate(S);
	C->Say("Starting Test: ");
	if (strcmp(S, "1")==0)
		rz = TestFork(C);
	  else if (strcmp(S, "2")==0)
		rz = TestPair(C);
	  else if (strcmp(S, "3")==0)
		rz = TestExec(C, argv[0]);
	  else if (strcmp(S, "exec")==0)
		return TestExec2(C);
	  else if (strcmp(S, "4")==0)
		rz = ThreadBash(C);
	  else if (strcmp(S, "5")==0)
		rz = TestPipe(C);
	  else
		rz = TestThread(C);
	PicoDestroy(C, "Finished");
	return rz;
}

