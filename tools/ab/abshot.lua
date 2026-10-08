-- abshot.lua -- side A of tools/ab/mame_ab.py: MAME's own machine, the same
-- input script and the same snapshots as the core's sms_headless (side B).
-- Everything is keyed on the frame count, which an identical machine
-- reaches identically.
--
--   AB_FRAMES  comma-separated frame numbers to snapshot (the last one exits)
--   AB_INPUT   optional file of "frame port mask" lines (mask hex; port 0/1
--              the joypads in SMS_PAD_* bits, 2 Pause, 3 Reset/Rapid)
--   AB_RAMDUMP optional file: the 8K of work RAM, written at the last frame
local frames = {}
for f in string.gmatch(os.getenv("AB_FRAMES") or "120", "[^,]+") do
  frames[tonumber(f)] = true
end
local last = 0
for f, _ in pairs(frames) do if f > last then last = f end end

local events = {}
local input = os.getenv("AB_INPUT")
if input and input ~= "" then
  for line in io.lines(input) do
    local fr, port, mask = line:match("^(%d+)%s+(%d+)%s+(%x+)")
    if fr then events[#events + 1] = { tonumber(fr), tonumber(port), tonumber(mask, 16) } end
  end
end

local ports = manager.machine.ioport.ports
local function field(tag, name)
  local p = ports[tag]
  return p and p.fields[name]
end
local pads = {
  { field(":ctrl1:mspad:JOYPAD", "P1 Up") , field(":ctrl1:mspad:JOYPAD", "P1 Down"),
    field(":ctrl1:mspad:JOYPAD", "P1 Left"), field(":ctrl1:mspad:JOYPAD", "P1 Right"),
    field(":ctrl1:mspad:JOYPAD", "P1 Button 1"), field(":ctrl1:mspad:JOYPAD", "P1 Button 2") },
  { field(":ctrl2:mspad:JOYPAD", "P2 Up") , field(":ctrl2:mspad:JOYPAD", "P2 Down"),
    field(":ctrl2:mspad:JOYPAD", "P2 Left"), field(":ctrl2:mspad:JOYPAD", "P2 Right"),
    field(":ctrl2:mspad:JOYPAD", "P2 Button 1"), field(":ctrl2:mspad:JOYPAD", "P2 Button 2") },
}
local pause = field(":PAUSE", "Pause")
local reset = field(":RESET", "Reset Button") or field(":RAPID", "Rapid Button")

local frame, ev = 0, 1
_G.__ab_sub = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if frames[frame] then
    manager.machine.screens[":screen"]:snapshot()
  end
  while ev <= #events and events[ev][1] <= frame do
    local e = events[ev]
    if e[2] <= 1 then
      for b = 0, 5 do
        local f = pads[e[2] + 1][b + 1]
        if f then f:set_value(((e[3] >> b) & 1) == 1 and 1 or 0) end
      end
    elseif e[2] == 2 and pause then
      pause:set_value(e[3] ~= 0 and 1 or 0)
    elseif e[2] == 3 and reset then
      reset:set_value(e[3] ~= 0 and 1 or 0)
    end
    ev = ev + 1
  end
  if frame >= last then
    local dump = os.getenv("AB_RAMDUMP")
    if dump and dump ~= "" then
      local sp = manager.machine.devices[":maincpu"].spaces["program"]
      local f = io.open(dump, "wb")
      for a = 0xc000, 0xdfff do f:write(string.char(sp:read_u8(a))) end
      f:close()
    end
    manager.machine:exit()
  end
end)
