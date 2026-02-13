#include "core/uuid.h"
#include <random>

using namespace Ember;

static std::random_device random_device;
static std::mt19937_64 engine(random_device());
static std::uniform_int_distribution<u64> distribution;

UUID::UUID() : m_uuid(distribution(engine)) {}

UUID::UUID(u64 uuid) : m_uuid(uuid) {}
