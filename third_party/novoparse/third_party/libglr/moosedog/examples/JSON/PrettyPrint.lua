-- PrettyPrinter.lua
-- Query: walk the JsonValue AST and emit formatted JSON text.
--
-- Called by the query runner as:
--   local pp = require("PrettyPrinter")
--   io.write(pp.run(absyn.root, { indent = "  ", max_width = 80 }))

local M = {}

-- ── helpers ───────────────────────────────────────────────────────────────────

-- Re-encode a decoded string value to JSON string syntax.
local function escape(s)
  return (s:gsub('[\x00-\x1f"\\]', function(c)
    local map = {
      ['"']  = '\\"',  ['\\'] = '\\\\',
      ['\b'] = '\\b',  ['\f'] = '\\f',
      ['\n'] = '\\n',  ['\r'] = '\\r',
      ['\t'] = '\\t',
    }
    return map[c] or ('\\u%04x'):format(c:byte())
  end))
end

local function num_repr(n)
  if n ~= n             then return '"NaN"'        end  -- not RFC 8259 valid;
  if n ==  math.huge    then return '"Infinity"'   end  -- quote rather than
  if n == -math.huge    then return '"-Infinity"'  end  -- crash the printer.
  if n == math.floor(n) and math.abs(n) < 1e15 then
    return ('%.0f'):format(n)
  end
  return ('%.17g'):format(n)
end

-- An array qualifies for the flat "[1, 2, 3]" layout when every element
-- is a scalar and the whole thing fits within the remaining line width.
local function flat_ok(elements, budget)
  local width = 2  -- "[]"
  for _, el in ipairs(elements) do
    local k = el.kind
    if k == "object" or k == "array" then return false end
    width = width + (k == "string" and #el.value + 4 or 8)
    if width > budget then return false end
  end
  return true
end

-- ── visitor ───────────────────────────────────────────────────────────────────

local Visitor = {}
Visitor.__index = Visitor

function Visitor.new(opts)
  opts = opts or {}
  return setmetatable({
    tab       = opts.indent    or "  ",
    max_width = opts.max_width or 80,
    depth     = 0,
    buf       = {},
  }, Visitor)
end

local function push(v, s) v.buf[#v.buf + 1] = s end
local function ind(v)     return v.tab:rep(v.depth) end

-- ── node handlers (the "query patterns") ─────────────────────────────────────

local handlers = {}

handlers.string = function(v, node)
  push(v, '"' .. escape(node.value) .. '"')
end

handlers.number = function(v, node)
  push(v, num_repr(node.value))
end

handlers.bool = function(v, node)
  push(v, node.value and "true" or "false")
end

handlers.null = function(_, _)
  -- push is in dispatch; kept here for symmetry
end

handlers.object = function(v, node)
  -- collect linked list → array so we know the last index
  local list = {}
  local m = node.members
  while m do list[#list + 1] = m; m = m.next end

  if #list == 0 then push(v, "{}"); return end

  push(v, "{\n")
  v.depth = v.depth + 1
  for i, member in ipairs(list) do
    push(v, ind(v))
    push(v, '"' .. escape(member.key.value) .. '": ')
    v:visit(member.value)
    if i < #list then push(v, ",") end
    push(v, "\n")
  end
  v.depth = v.depth - 1
  push(v, ind(v) .. "}")
end

handlers.array = function(v, node)
  local els = node.elements

  if #els == 0 then push(v, "[]"); return end

  local budget = v.max_width - v.depth * #v.tab
  if flat_ok(els, budget) then
    push(v, "[")
    for i, el in ipairs(els) do
      v:visit(el)
      if i < #els then push(v, ", ") end
    end
    push(v, "]")
    return
  end

  push(v, "[\n")
  v.depth = v.depth + 1
  for i, el in ipairs(els) do
    push(v, ind(v))
    v:visit(el)
    if i < #els then push(v, ",") end
    push(v, "\n")
  end
  v.depth = v.depth - 1
  push(v, ind(v) .. "]")
end

-- ── dispatch ──────────────────────────────────────────────────────────────────

function Visitor:visit(node)
  local h = handlers[node.kind]
  if h then
    h(self, node)
  else
    push(self, ("/* unknown kind: %s */"):format(tostring(node.kind)))
  end
end

-- ── public ────────────────────────────────────────────────────────────────────

--- @param root   table   JsonValue (JSONAbsyn.root)
--- @param opts?  table   { indent: string, max_width: number }
--- @return string
function M.run(root, opts)
  local v = Visitor.new(opts)
  v:visit(root)
  return table.concat(v.buf)
end

return M

