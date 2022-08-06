-- support takeoff and landing on moving platforms for VTOL planes

local PARAM_TABLE_KEY = 7
local PARAM_TABLE_PREFIX = "RTL2_"

local MODE_RTL = 11

local ALT_FRAME_ABSOLUTE = 0

-- bind a parameter to a variable
function bind_param(name)
   local p = Parameter()
   assert(p:init(name), string.format('could not find %s parameter', name))
   return p
end

-- add a parameter and bind it to a variable
function bind_add_param(name, idx, default_value)
   assert(param:add_param(PARAM_TABLE_KEY, idx, name, default_value), string.format('could not add param %s', name))
   return bind_param(PARAM_TABLE_PREFIX .. name)
end

-- setup SHIP specific parameters
assert(param:add_table(PARAM_TABLE_KEY, PARAM_TABLE_PREFIX, 1), 'could not add param table')
RTL2_ENABLE     = bind_add_param('ENABLE', 1)

-- other parameters
WP_LOITER_RAD   = bind_param("WP_LOITER_RAD")
RTL_RADIUS      = bind_param("RTL_RADIUS")

-- an auth ID to disallow arming when we don't have the beacon
--local auth_id = arming:get_aux_auth_id()
--arming:set_aux_auth_failed(auth_id, "Ship: no beacon")

-- current target
local target_pos = Location()
local current_pos = Location()

-- square a variable
function sq(v)
   return v*v
end

-- check key parameters
function check_parameters()
  --[[
     parameter values which are auto-set on startup
  --]]
   local key_params = {
      FOLL_ENABLE = 1,
      FOLL_OFS_TYPE = 1,
      FOLL_ALT_TYPE = 0,
   }

   for p, v in pairs(key_params) do
      local current = param:get(p)
      assert(current, string.format("Parameter %s not found", p))
      if math.abs(v-current) > 0.001 then
         param:set_and_save(p, v)
         gcs:send_text(0,string.format("Parameter %s set to %.2f was %.2f", p, v, current))
      end
   end
end

-- get holdoff distance
function get_holdoff_radius()
   if RTL_RADIUS:get() ~= 0 then
      return RTL_RADIUS:get()
   end
   return WP_LOITER_RAD:get()
end

function wrap_360(angle)
   local res = math.fmod(angle, 360.0)
    if res < 0 then
        res = res + 360.0
    end
    return res
end

function wrap_180(angle)
    local res = wrap_360(angle)
    if res > 180 then
       res = res - 360
    end
    return res
end

--[[
  update automatic beacon offsets
--]]

-- main update function
function update()

   if RTL2_ENABLE:get() < 1 then
      return
   end

   gcs:send_text(0, "Starting emergency RTL")

   current_pos = ahrs:get_position()
   if not current_pos then
      return
   end

   local home_position = ahrs:get_home();
   local landing_position = home_position.offset(10,20)

   -- ahrs:set_home(target_pos)
   --local next_WP = vehicle:get_target_location()
   --if not next_WP then
      -- not in a flight mode with a target location
   --   return
   --end

   local vehicle_mode = vehicle:get_mode()
   if vehicle_mode == MODE_RTL then
      -- vehicle:set_target_pos_NED(arget_pos, use_yaw, yaw_deg, use_yaw_rate, yaw_rate_degs, yaw_relative, terrain_alt) end
      vehicle:update_target_location(home_position, landing_position)
   end

end

function loop()
   update()
   -- run at 20Hz
   return loop, 50
end

check_parameters()

-- wrapper around update(). This calls update() at 20Hz,
-- and if update faults then an error is displayed, but the script is not
-- stopped
function protected_wrapper()
  local success, err = pcall(update)
  if not success then
     gcs:send_text(0, "Internal Error: " .. err)
     -- when we fault we run the update function again after 1s, slowing it
     -- down a bit so we don't flood the console with errors
     return protected_wrapper, 1000
  end
  return protected_wrapper, 50
end

-- start running update loop
return protected_wrapper()

