-- moosedog.lua -- shared helpers for Moosedog Lua queries.
-- Each query (e.g. PrettyPrint.lua, Linter.lua) exposes M.run(root, opts).
local M = {}
function M.members_to_array(obj)
  local t = {}
  local m = obj.members
  while m do t[#t + 1] = m; m = m.next end
  return t
end
function M.is_scalar(kind)
  return kind == "string" or kind == "number" or kind == "bool" or kind == "null"
end
return M
