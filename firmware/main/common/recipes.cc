#include "common/recipes.h"

namespace recipes {

namespace {
#include "common/recipes_data.inc"
}  // namespace

int Count() { return static_cast<int>(sizeof(kRecipes) / sizeof(kRecipes[0])); }

const Recipe& Get(int index) {
    const int n = Count();
    return kRecipes[((index % n) + n) % n];
}

}  // namespace recipes
