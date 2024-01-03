


#define PICO_IMPLEMENTATION
#include "PicoMsg.h"
#include <unistd.h>
#include <vector>
using std::vector; 



void* Query (PicoComms* M) {
	M->Conf.Name = "Query";
	PicoMsgSay(M, "Sending");
	PicoMsgCSend(M, "mary had a little lamb");
	const int Stack = 10; const int Pudge = 4096;
	vector<char> abcd(Stack*Pudge);
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] = rand();
	}
	
	vector<PicoMessage> Sent;
	char* abc = &abcd[0];
	int Remain = Stack*Pudge;
	while (Remain > 0) {
		if (PicoMsgErr(M)) return 0;

		int n = rand() % Pudge;
		if (n > Remain) n = Remain;
		Remain -= n;
		Sent.push_back({abc, n});
		PicoMsgSend(M, {abc, n});
		abc += n;
	}
	
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] ++;
	}

	PicoMsgSay(M, "Comparing");

	for (auto v: Sent) {
		auto OK = PicoMsgGet(M, 5.0);
		if (!OK)								return 0;
		if (OK.Length != v.Length)				return PicoMsgSay(M, "bad length");
		int diff = memcmp(v.Data, OK.Data, v.Length);
		if (diff)								return PicoMsgSay(M, "netdown");
		abc += v.Length;
		PicoMsgSay(M, "Passed");
		free(OK.Data);
	}
	PicoMsgSay(M, "QD");
	return 0;
}


void Respond (PicoComms* M) {
	M->Conf.Name = "Respond";
	sleep(1); // give it a chance to receive
	auto Mary = PicoMsgGet(M);
	if (Mary) {
		puts(Mary.Data);
		free(Mary.Data);
	}
	
	while (!PicoMsgErr(M)) {
		auto Msg = PicoMsgGet(M, 10.0);
		if (!Msg) break;

		for (int i = 0; i < Msg.Length; i++) {
			(Msg.Data)[i]++;
		}
		PicoMsgSend(M, Msg); // send will "own" the malloc'd data in Msg
	}
}


int main (int argc, const char * argv[]) {
	auto M = PicoMsgComms(PicoNoisy);
	int PID = PicoMsgFork(M);
	if (PID < 0)
		return -PID;
	if (PID)
		Query(M);
	  else
		Respond(M);
	PicoMsgDestroy(M);		
	return 0;
}

