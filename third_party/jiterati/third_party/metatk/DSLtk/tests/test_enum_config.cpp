#define MAGIC_ENUM_RANGE_MIN -256
#define MAGIC_ENUM_RANGE_MAX 512
#define MAGIC_ENUM_ENABLE_HASH
#define MAGIC_ENUM_ENABLE_HASH_SWITCH
#include "DSLtk.hpp"

enum class Wide { Low = -256, Mid = 256, High = 512, Outside = 513 };
enum class Narrow { Low = -5, Mid = 0, High = 5 };

template <> struct dsl::customize::enum_range<Narrow>
{
  static constexpr int min = -1;
  static constexpr int max = 1;
};

static_assert (dsl::enum_count<Wide> () == 3);
static_assert (dsl::enum_cast<Wide> ("Mid") == Wide::Mid);
static_assert (dsl::enum_cast<Wide> (-256) == Wide::Low);
static_assert (dsl::enum_cast<Wide> ("hIgH", dsl::case_insensitive) == Wide::High);
static_assert (!dsl::enum_cast<Wide> (513));
static_assert (dsl::enum_name<Wide::Outside> () == "Outside");
static_assert (dsl::enum_index (Wide::High) == 2);
static_assert (dsl::enum_count<Narrow> () == 1);
static_assert (!dsl::enum_contains (Narrow::Low));
static_assert (dsl::enum_switch ([] (auto value) { return dsl::enum_integer (value ()); },
                               Wide::Mid) == 256);

int
main ()
{
  return dsl::enum_cast<Wide> ("Low") == Wide::Low ? 0 : 1;
}
