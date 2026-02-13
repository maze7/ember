#pragma once

#include "core/common.h"

namespace Ember
{
	class UUID
	{
	public:
		UUID();

		explicit UUID(u64 uuid);

		operator u64() const { return m_uuid; }

		// Let compiler generate comparison methods
		auto operator<=>(const UUID&) const = default;

	private:
		u64 m_uuid;
	};
}

namespace std
{
	template <>
	struct hash<Ember::UUID>
	{
		std::size_t operator() (const Ember::UUID& uuid) const noexcept {
			return hash<u64>()((u64)uuid);
		}
	};
}
