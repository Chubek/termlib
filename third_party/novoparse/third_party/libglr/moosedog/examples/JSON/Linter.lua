-- Linter.lua
-- Query: walk the JsonValue AST and collect diagnostic issues.
--
-- Called by the query runner as:
--   local lint   = require("Linter")
--   local issues = lint.run(absyn.root, opts?)
--   io.write(lint.format(issues))
--
-- Each issue:
--   { level = "warn"|"error", code = "JXXX", path = string, message = string }

local M = {}

-- ── defaults (all overridable via opts) ───────────────────────────────────────

local DEFAULTS = {
  max_depth         = 20,     -- nesting levels before J001
  max_string_bytes  = 4096,   -- string value byte length before J030
  max_key_bytes     = 128,    -- object key byte length before J012
  max_array_len     = 1000,   -- element count before J021
  duplicate_keys    = true,   -- J011 / J011-E
  numeric_keys      = true,   -- J013: keys like "0", "1" …
  empty_collections = true,   -- J010, J020
  mixed_arrays      = true,   -- J022: different non-null types in one array
}

-- ── helpers ───────────────────────────────────────────────────────────────────

local function members_array(obj)
  local t = {}
  local m = obj.members
  while m do t[#t + 1] = m; m = m.next end
  return t
end

local function is_integer_key(s)
  return s:match("^-?%d+$") ~= nil
end

-- ── walker ────────────────────────────────────────────────────────────────────

local Walker = {}
Walker.__index = Walker

function Walker.new(opts)
  opts = opts or {}
  local cfg = {}
  for k, default in pairs(DEFAULTS) do
    cfg[k] = (opts[k] ~= nil) and opts[k] or default
  end
  return setmetatable({
    cfg    = cfg,
    issues = {},
    depth  = 0,
    path   = { "$" },   -- JSONPath-style breadcrumb
  }, Walker)
end

local function current_path(w)
  return table.concat(w.path)
end

local function issue(w, level, code, msg)
  w.issues[#w.issues + 1] = {
    level   = level,
    code    = code,
    path    = current_path(w),
    message = msg,
  }
end

local warn  = function(w, c, m) issue(w, "warn",  c, m) end
local error_ = function(w, c, m) issue(w, "error", c, m) end

-- ── node handlers (query patterns) ───────────────────────────────────────────

local handlers = {}

handlers.object = function(w, node)
  local members = members_array(node)

  if #members == 0 and w.cfg.empty_collections then
    warn(w, "J010", "empty object")
  end

  -- duplicate keys
  if w.cfg.duplicate_keys then
    local seen = {}
    for _, m in ipairs(members) do
      local k = m.key.value
      if seen[k] then
        -- second and further occurrences are errors — last-value-wins silently
        -- breaks expectations; flag them loudly.
        error_(w, "J011", ('duplicate key "%s"'):format(k))
      end
      seen[k] = true
    end
  end

  for _, m in ipairs(members) do
    local k = m.key.value

    if #k > w.cfg.max_key_bytes then
      warn(w, "J012",
        ('key "%s…" is %d bytes (max %d)'):format(
          k:sub(1, 32), #k, w.cfg.max_key_bytes))
    end

    if w.cfg.numeric_keys and is_integer_key(k) then
      warn(w, "J013",
        ('key "%s" is a numeric string — consider an array'):format(k))
    end

    -- push path segment, recurse, pop
    local seg = '["' .. k:gsub('"', '\\"') .. '"]'
    w.path[#w.path + 1] = seg
    w:walk(m.value)
    w.path[#w.path] = nil
  end
end

handlers.array = function(w, node)
  local els = node.elements

  if #els == 0 and w.cfg.empty_collections then
    warn(w, "J020", "empty array")
  end

  if #els > w.cfg.max_array_len then
    warn(w, "J021",
      ('%d elements exceeds max_array_len %d'):format(
        #els, w.cfg.max_array_len))
  end

  if w.cfg.mixed_arrays then
    local base_kind = nil
    for _, el in ipairs(els) do
      if el.kind ~= "null" then
        if base_kind == nil then
          base_kind = el.kind
        elseif el.kind ~= base_kind then
          warn(w, "J022",
            ('mixed element types ("%s" and "%s")'):format(base_kind, el.kind))
          break
        end
      end
    end
  end

  for i, el in ipairs(els) do
    w.path[#w.path + 1] = '[' .. (i - 1) .. ']'
    w:walk(el)
    w.path[#w.path] = nil
  end
end

handlers.string = function(w, node)
  local len = #node.value
  if len > w.cfg.max_string_bytes then
    warn(w, "J030",
      ('string is %d bytes (max %d)'):format(len, w.cfg.max_string_bytes))
  end
end

handlers.number = function(w, node)
  local n = node.value
  if n ~= n then
    error_(w, "J040", "NaN is not valid in RFC 8259 JSON")
  elseif math.abs(n) == math.huge then
    error_(w, "J041", "±Infinity is not valid in RFC 8259 JSON")
  end
end

-- bool and null carry no lintable content; no handler needed.

-- ── dispatch + depth guard ────────────────────────────────────────────────────

function Walker:walk(node)
  self.depth = self.depth + 1

  if self.depth > self.cfg.max_depth then
    warn(self, "J001",
      ('nesting depth %d exceeds max_depth %d'):format(
        self.depth, self.cfg.max_depth))
    -- stop descending to avoid O(n) cascade warnings for deep structures
    self.depth = self.depth - 1
    return
  end

  local h = handlers[node.kind]
  if h then h(self, node) end

  self.depth = self.depth - 1
end

-- ── public ────────────────────────────────────────────────────────────────────

--- @param root   table    JsonValue (JSONAbsyn.root)
--- @param opts?  table    override DEFAULTS
--- @return table[]        array of issue records
function M.run(root, opts)
  local w = Walker.new(opts)
  w:walk(root)
  -- stable sort: errors before warnings, then by path
  table.sort(w.issues, function(a, b)
    if a.level ~= b.level then
      return a.level == "error"  -- errors first
    end
    return a.path < b.path
  end)
  return w.issues
end

--- Render issues as a human-readable report string.
--- @param issues table[]
--- @return string
function M.format(issues)
  if #issues == 0 then return "No issues.\n" end
  local lines = {}
  for _, iss in ipairs(issues) do
    lines[#lines + 1] = ('[%s] %s  %s  %s'):format(
      iss.level:upper(), iss.code, iss.path, iss.message)
  end
  return table.concat(lines, "\n") .. "\n"
end

return M

