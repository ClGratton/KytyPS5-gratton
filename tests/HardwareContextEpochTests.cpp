#include "graphics/guest_gpu/hardwareContext.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HardwareContextEpochTests: failed: %s\n", message);
		std::abort();
	}
}

void TestContextEpochs() {
	HW::Context ctx;
	Check(ctx.Epoch() == 0, "a fresh context has epoch 0");
	std::set<uint64_t> seen {0};
	const auto         step = [&](const char* what) {
        const auto e = ctx.Epoch();
        Check(seen.insert(e).second, what);
    };
	ctx.SetRenderTargetMask(0x0f);
	step("SetRenderTargetMask takes a new epoch");
	ctx.SetBlendColor({});
	step("SetBlendColor takes a new epoch");
	ctx.SetViewportScaleOffset(0, 1, 2, 3, 4, 5, 6);
	step("a multi-line viewport setter takes a new epoch");
	ctx.SetViewportScissor(0, 1, 2, 3, 4, true);
	step("the scissor setter takes a new epoch");
	ctx.SetColorBase(3, {});
	step("a per-slot setter takes a new epoch");
	ctx.SetShaderMask(0x1234);
	step("a shader-register setter takes a new epoch");
	ctx.SetPsInputSettings(5, 7);
	step("SetPsInputSettings takes a new epoch");
	// Even a setter that writes the same value moves the epoch (the epoch is a proof of "nothing was set", not of "nothing changed").
	const auto before = ctx.Epoch();
	ctx.SetShaderMask(0x1234);
	Check(ctx.Epoch() != before, "setting the same value keeps the old epoch");

	// A copy has the epoch of the content it copies, and a setter on the copy leaves the original alone.
	HW::Context copy = ctx;
	Check(copy.Epoch() == ctx.Epoch() && copy.SameRegisters(ctx), "a copy differs from its original");
	copy.SetRenderTargetMask(0xf0);
	Check(copy.Epoch() != ctx.Epoch() && !copy.SameRegisters(ctx), "a changed copy kept the epoch");
	// Restoring a saved context restores its epoch; the next change is new again.
	const auto saved_epoch = ctx.Epoch();
	HW::Context live = ctx;
	live.SetRenderTargetMask(1);
	const auto changed = live.Epoch();
	live = ctx;
	Check(live.Epoch() == saved_epoch, "restoring a context did not restore its epoch");
	live.SetRenderTargetMask(2);
	Check(live.Epoch() != changed && live.Epoch() != saved_epoch, "an epoch was reused after a restore");
	live.Reset();
	Check(live.Epoch() == 0, "Reset does not give the default epoch");
}

void TestUserConfigEpochs() {
	HW::UserConfig uc;
	const auto     epoch = uc.Epoch();
	uc.SetIndexOffset(7);
	uc.SetObjectId(9);
	Check(uc.Epoch() == epoch, "the per-draw counters moved the static epoch");
	Check(uc.DrawEpoch() != 0, "the per-draw counters did not move the draw epoch");
	uc.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
	Check(uc.Epoch() != epoch, "SetPrimitiveType did not move the epoch");
	HW::UserConfig other = uc;
	Check(other.Epoch() == uc.Epoch() && other.SameStaticRegisters(uc), "a copy of the user config differs");
	other.SetPrimitiveResetControl(3);
	Check(!other.SameStaticRegisters(uc) && other.Epoch() != uc.Epoch(), "a changed user config kept its epoch");
}

void TestShaderEpochs() {
	HW::Shader sh;
	const auto e0 = sh.Epoch();
	sh.SetPsUserSgpr(0, 5, HW::UserSgprType::Unknown);
	sh.SetGsUserSgpr(1, 6, HW::UserSgprType::Unknown);
	Check(sh.Epoch() == e0, "user SGPRs moved the stage-register epoch");
	Check(sh.PsUserEpoch() != 0 && sh.GsUserEpoch() != 0 && sh.CsUserEpoch() == 0 && sh.HsUserEpoch() == 0,
	      "user SGPR epochs are not per stage");
	sh.SetPsShaderBase(0x1000);
	Check(sh.Epoch() != e0, "SetPsShaderBase did not move the epoch");
	HW::Shader other = sh;
	Check(other.SameStageRegisters(sh), "a copy of the shader registers differs");
	other.SetGsShaderBase(0x2000);
	Check(!other.SameStageRegisters(sh) && other.Epoch() != sh.Epoch(), "a changed shader kept its epoch");
}

} // namespace

int main() {
	TestContextEpochs();
	TestUserConfigEpochs();
	TestShaderEpochs();
	std::printf("HardwareContextEpochTests: ok\n");
	return 0;
}
