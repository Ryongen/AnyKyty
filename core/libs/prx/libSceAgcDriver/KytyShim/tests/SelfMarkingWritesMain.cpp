#include "SelfMarkingWrites.hpp"

#include "prx/libSceAgc/Cb/include/Sync.hpp"
#include "prx/libSceAgc/DcbFlow/include/Control.hpp"
#include "prx/libSceAgc/Patch/include/Rewind.hpp"

int main() {
	const KytySelfMarkingAgc agc {&sceAgcCbNop_nid_postfix, &sceAgcDcbRewind, &sceAgcRewindPatchSetRewindState};
	return RunKytySelfMarkingWritesTest(&agc);
}
