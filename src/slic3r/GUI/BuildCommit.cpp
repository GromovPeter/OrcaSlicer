#include "BuildCommit.hpp"
#include "git_commit_hash.h"

namespace Slic3r { namespace GUI {

const char *const build_commit_hash  = GIT_COMMIT_HASH;
const char *const build_commit_label = GIT_COMMIT_HASH GIT_COMMIT_SUFFIX;
const char *const adaptive_build_label = "Adaptive Temperature B11";

}} // namespace Slic3r::GUI
