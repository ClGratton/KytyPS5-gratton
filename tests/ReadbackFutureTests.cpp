#include "graphics/host_gpu/renderer/cache/readbackFuture.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ReadbackFutureTests: failed: %s\n", message);
		std::abort();
	}
}

void TestStartDecision() {
	Check(DecideFutureStart(0, false) == FutureStart::Sync, "nothing scheduled, nothing in flight: old path");
	Check(DecideFutureStart(1, false) == FutureStart::Wait, "a new copy: wait for it");
	Check(DecideFutureStart(0, true) == FutureStart::Wait, "only an older copy in flight: wait for it");
	Check(DecideFutureStart(3, true) == FutureStart::Wait, "both: wait");
}

void TestRounds() {
	Check(NextFutureRound(0, true) == FutureRound::Done, "readable after the first round: done");
	Check(NextFutureRound(0, false) == FutureRound::Retry, "a newer write after round 1: retry");
	Check(NextFutureRound(1, false) == FutureRound::Retry, "a newer write after round 2: retry");
	Check(NextFutureRound(2, false) == FutureRound::Fallback,
	      "a newer write after the last round: the old synchronous path, so a reader cannot starve");
	Check(NextFutureRound(2, true) == FutureRound::Done, "readable on the last round: done");
}

void TestContendedWindow() {
	Check(!IsContended(0, 5), "never marked: not contended");
	Check(IsContended(10, 11), "just marked: contended");
	Check(IsContended(10, 10 + kContendedStarts - 1), "still inside the window");
	Check(!IsContended(10, 10 + kContendedStarts), "the window ended");
}

void TestReleaseIsOneShotAcrossThreads() {
	auto       release = std::make_shared<FutureRelease>();
	const auto start   = std::chrono::steady_clock::now();
	std::thread releaser([release] {
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		release->Signal();
	});
	Check(!release->TryWait(), "not released yet");
	release->Wait();
	releaser.join();
	Check(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(25),
	      "Wait blocked until the release");
	Check(!release->TryWait(), "one-shot: consumed");
}

} // namespace

int main() {
	TestStartDecision();
	TestRounds();
	TestContendedWindow();
	TestReleaseIsOneShotAcrossThreads();
	std::printf("ReadbackFutureTests: ok\n");
	return 0;
}
