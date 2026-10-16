#include "graphics/host_gpu/renderer/image/compareView.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::ClassifyComparedView;
using Libs::Graphics::CompareViewReport;
using Libs::Graphics::CompareViewVerdict;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "CompareViewTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	// A slot that is not compared is nobody's business, whatever format its view has.
	Check(ClassifyComparedView(false, true) == CompareViewVerdict::NotCompared &&
	          ClassifyComparedView(false, false) == CompareViewVerdict::NotCompared,
	      "a slot without a comparison sampler was classified");
	// Compared: valid only when the view format supports depth comparison (a depth format
	// everywhere; R32_SFLOAT on AMD but not on NVIDIA).
	Check(ClassifyComparedView(true, true) == CompareViewVerdict::Supported,
	      "a view with the comparison feature was reported");
	Check(ClassifyComparedView(true, false) == CompareViewVerdict::Unsupported,
	      "R32_SFLOAT compared on a GPU without the feature was not detected");

	CompareViewReport report;
	Check(report.Note(CompareViewVerdict::Supported) == 0 &&
	          report.Note(CompareViewVerdict::NotCompared) == 0 && report.Unsupported() == 0,
	      "valid bindings were counted");
	Check(report.Note(CompareViewVerdict::Unsupported) == 1 &&
	          report.Note(CompareViewVerdict::Unsupported) == 2 && report.Unsupported() == 2,
	      "unsupported bindings were not counted in order");
	Check(CompareViewReport::ShouldLog(1) &&
	          CompareViewReport::ShouldLog(CompareViewReport::kLogLimit) &&
	          !CompareViewReport::ShouldLog(CompareViewReport::kLogLimit + 1) &&
	          !CompareViewReport::ShouldLog(0),
	      "the log limit is wrong");
	std::puts("CompareViewTests: all cases passed");
	return 0;
}
