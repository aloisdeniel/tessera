-- game.lua — Tic-Tac-Toe, played entirely inside the engine's embedded Lua VM.
--
-- The host does almost nothing: it loads the asset bundle (ttt.tsb) and this
-- script, then forwards board taps as events {name="tap", args={x, y}}. All
-- rules, animation staging and presentation live here. The chunk's top level
-- registers every def from bundle assets, then returns the game function; the
-- engine calls it once per event and plays the returned states IN ORDER, each
-- one waiting for the previous transition to settle — so a move is authored as
-- a sequence of beats (hover, drop, then a win cascade) rather than one jump.
--
-- Board cells are numbered 1..9, row-major over tile coords (-1..1, -1..1).
-- Marks are cards: a placed mark first appears airborne showing the card back,
-- then drops onto its tile while flipping face-up. Winning marks glow one by
-- one, and any tap after the round ends sweeps the cards off the board.

----------------------------------------------------------------------------
-- definitions — registered once, from bundle assets (see tools/pack_bundle.py)
----------------------------------------------------------------------------
local atlas     = tessera.register_atlas(tessera.asset("ttt_atlas.png"))
local font      = tessera.register_font(tessera.asset("Roboto-Regular.ttf"), 64)
local snd_place = tessera.register_sound(tessera.asset("place.wav"))
local snd_win   = tessera.register_sound(tessera.asset("win.wav"))

-- ttt_atlas.png is four 256px quadrants (see tool/generate_ttt_assets.py).
local UV_X    = { 0.0, 0.0, 0.5, 0.5 }   -- X card face
local UV_O    = { 0.5, 0.0, 1.0, 0.5 }   -- O card face
local UV_BACK = { 0.0, 0.5, 0.5, 1.0 }   -- concealing back pattern
local UV_TILE = { 0.5, 0.5, 1.0, 1.0 }   -- board tile top

-- Checkerboard from one texture, tinted light / dark.
local tile_light = tessera.register_tile_def{
  atlas = atlas, top = UV_TILE, thickness = 0.22,
  tint = { 0.94, 0.87, 0.72, 1.0 },
}
local tile_dark = tessera.register_tile_def{
  atlas = atlas, top = UV_TILE, thickness = 0.22,
  tint = { 0.62, 0.48, 0.34, 1.0 },
}

-- One card def per side; both conceal behind the same back pattern, so a mark
-- placed `hidden` shows the back until its reveal flip.
local function mark_def(uv)
  return tessera.register_card_def{
    visible_atlas = atlas, visible_uv = uv,
    hidden_atlas  = atlas, hidden_uv  = UV_BACK,
    back_atlas    = atlas, back_uv    = UV_BACK,
    width = 0.84, height = 0.84, thickness = 0.04, corner_radius = 0.12,
  }
end
local card_def = { mark_def(UV_X), mark_def(UV_O) }   -- indexed by side 1 / 2
local NAME     = { "X", "O" }

----------------------------------------------------------------------------
-- rules
----------------------------------------------------------------------------
local LINES = {
  { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 },   -- rows
  { 1, 4, 7 }, { 2, 5, 8 }, { 3, 6, 9 },   -- columns
  { 1, 5, 9 }, { 3, 5, 7 },                -- diagonals
}

local board       -- board[1..9]: 0 empty, 1 X, 2 O
local player      -- side to move (1 or 2)
local winner      -- nil = playing, 0 = draw, 1/2 = that side won
local win_line    -- the winning {a, b, c} cells when winner is 1/2
local last_cell   -- cell of the move just played (0 = none yet)

local function new_round()
  board, player, winner, win_line, last_cell = {}, 1, nil, nil, 0
  for i = 1, 9 do board[i] = 0 end
end
new_round()

local function cell_x(i) return (i - 1) % 3 - 1 end
local function cell_y(i) return math.floor((i - 1) / 3) - 1 end

-- Tap coords (doubles from the host) -> cell index, or nil off the board.
local function cell_at(x, y)
  if not x or not y then return nil end
  x, y = math.floor(x + 0.5), math.floor(y + 0.5)
  if x < -1 or x > 1 or y < -1 or y > 1 then return nil end
  return (y + 1) * 3 + (x + 1) + 1
end

-- The round's result for the current board: winner 1/2 plus its line, 0 for a
-- full board with no line (draw), or nil while the round is still open.
local function round_result()
  for _, l in ipairs(LINES) do
    local a = board[l[1]]
    if a ~= 0 and a == board[l[2]] and a == board[l[3]] then return a, l end
  end
  for i = 1, 9 do
    if board[i] == 0 then return nil end
  end
  return 0
end

----------------------------------------------------------------------------
-- presentation — one full state snapshot per beat
----------------------------------------------------------------------------
local function status_text()
  if winner == nil then return NAME[player] .. " to play" end
  if winner == 0 then return "A draw. Tap to play again" end
  return NAME[winner] .. " wins! Tap to play again"
end

-- Build the complete scene for the current game vars. `o` stages a beat:
--   o.hover    cell whose mark is still airborne (back up, above its tile)
--   o.parked   marks swept off the board (the between-rounds beat)
--   o.glow     how many winning marks glow already (0..3, win cascade)
--   o.status   status label override (default: status_text())
--   o.distance camera distance override (the win beat pushes in)
--   o.bare     tiles only — the opening beat, before the labels fade in
local function scene(o)
  o = o or {}
  local st = {
    camera = {
      -- yaw 0 keeps the flat ground labels upright; the focus sits a touch
      -- past centre so both edge labels stay framed
      mode = "orbit", focus = { 0, 0.25 },
      distance = o.distance or 7.0, yaw = 0, pitch = 1.05, fov = 0.7,
    },
    tiles = {}, cards = {}, overlays = {}, labels = {}, highlights = {},
  }

  for i = 1, 9 do
    st.tiles[i] = {
      x = cell_x(i), y = cell_y(i), id = i,
      def = (cell_x(i) + cell_y(i)) % 2 == 0 and tile_light or tile_dark,
    }
  end

  for i = 1, 9 do
    local side = board[i]
    if side ~= 0 then
      local card = {
        id = 10 + i, def = card_def[side],
        position = { cell_x(i), 0.02, cell_y(i) },
      }
      if o.hover == i then          -- just played: airborne, back showing
        card.position[2] = 1.1
        card.hidden = true
      elseif o.parked then          -- between rounds: slid off the board edge
        card.position = { 3.1, 0.4, (i - 5) * 0.5 }
      end
      st.cards[#st.cards + 1] = card
    end
  end

  -- A gold ring marks the latest move once its card has landed.
  if last_cell > 0 and not o.hover and not o.parked then
    st.overlays[1] = {
      x = cell_x(last_cell), y = cell_y(last_cell), shape = "ring",
      tint = { 1.0, 0.82, 0.25, 0.8 },
      pulse_s = 1.4, pulse_alpha_min = 0.35, pulse_alpha_max = 0.8,
    }
  end

  -- The win cascade lights the line's marks up one beat at a time.
  if win_line then
    for k = 1, math.min(o.glow or 0, 3) do
      st.highlights[k] = {
        target = 10 + win_line[k], kind = "card", style = "glow",
        color = { 1.0, 0.84, 0.3, 1.0 }, thickness = 18,
        pulse_s = 1.2, pulse_min = 0.55, pulse_max = 1.0,
      }
    end
  end

  if not o.bare then
    st.labels[1] = {
      id = 1, font = font, text = o.status or status_text(),
      position = { 0, 0.02, 2.2 }, size = 0.42, color = { 1, 1, 1, 0.92 },
    }
    st.labels[2] = {
      id = 2, font = font, text = "TIC - TAC - TOE",
      position = { 0, 0.02, -2.35 }, size = 0.5, color = { 1, 0.9, 0.6, 0.85 },
    }
  end
  return st
end

----------------------------------------------------------------------------
-- the game function — one event in, an ordered sequence of beats out
----------------------------------------------------------------------------
return function(event)
  if event.name == "start" then
    new_round()
    -- The board rises first; the labels fade in once it has settled.
    return { scene{ bare = true }, scene{} }
  end

  if event.name ~= "tap" then return nil end
  local args = event.args or {}

  -- Any tap after the round ended starts the next one: sweep the old marks
  -- off the board, then raise a fresh empty grid.
  if winner then
    local sweep = scene{ parked = true, status = "Next round..." }
    new_round()
    return { sweep, scene{} }
  end

  local cell = cell_at(args[1], args[2])
  if not cell or board[cell] ~= 0 then return nil end

  board[cell] = player
  last_cell = cell
  winner, win_line = round_result()
  tessera.play_sound(snd_place)

  -- Beat 1: the mark appears airborne, back up. Beat 2: it drops and flips.
  local beats = { scene{ hover = cell } }
  if winner == nil then
    player = 3 - player
    beats[#beats + 1] = scene{}
  elseif winner == 0 then
    beats[#beats + 1] = scene{}
  else
    tessera.play_sound(snd_win)
    beats[#beats + 1] = scene{ status = "Three in a row!" }
    beats[#beats + 1] = scene{ glow = 1, status = "Three in a row!" }
    beats[#beats + 1] = scene{ glow = 2, status = "Three in a row!" }
    beats[#beats + 1] = scene{ glow = 3, distance = 6.5 }
  end
  return beats
end
