/// @file    AP_Rally.h
/// @brief   Handles rally point storage and retrieval.
#include "AP_Rally.h"

#include <AP_AHRS/AP_AHRS.h>
#include <AP_Logger/AP_Logger.h>
#include <StorageManager/StorageManager.h>

#if HAL_RALLY_ENABLED
// storage object
StorageAccess AP_Rally::_storage(StorageManager::StorageRally);

assert_storage_size<RallyLocation, 15> _assert_storage_size_RallyLocation;

#if APM_BUILD_COPTER_OR_HELI
  #define RALLY_LIMIT_KM_DEFAULT 0.3f
  #define RALLY_INCLUDE_HOME_DEFAULT 1
#elif APM_BUILD_TYPE(APM_BUILD_ArduPlane)
  #define RALLY_LIMIT_KM_DEFAULT 5.0f
  #define RALLY_INCLUDE_HOME_DEFAULT 0
#elif APM_BUILD_TYPE(APM_BUILD_Rover)
  #define RALLY_LIMIT_KM_DEFAULT 0.5f
  #define RALLY_INCLUDE_HOME_DEFAULT 1
#else
  #define RALLY_LIMIT_KM_DEFAULT 1.0f
  #define RALLY_INCLUDE_HOME_DEFAULT 0
#endif

const AP_Param::GroupInfo AP_Rally::var_info[] = {
    // @Param: TOTAL
    // @DisplayName: Rally Total
    // @Description: Number of rally points currently loaded
    // @User: Advanced
    AP_GROUPINFO("TOTAL", 0, AP_Rally, _rally_point_total_count, 0),

    // @Param: LIMIT_KM
    // @DisplayName: Rally Limit
    // @Description: Maximum distance to rally point. If the closest rally point is more than this number of kilometers from the current position and the home location is closer than any of the rally points from the current position then do RTL to home rather than to the closest rally point. This prevents a leftover rally point from a different airfield being used accidentally. If this is set to 0 then the closest rally point is always used.
    // @User: Advanced
    // @Units: km
    // @Increment: 0.1
    AP_GROUPINFO("LIMIT_KM", 1, AP_Rally, _rally_limit_km, RALLY_LIMIT_KM_DEFAULT),

    // @Param: INCL_HOME
    // @DisplayName: Rally Include Home
    // @Description: Controls if Home is included as a Rally point (i.e. as a safe landing place) for RTL
    // @User: Standard
    // @Values: 0:DoNotIncludeHome,1:IncludeHome
    AP_GROUPINFO("INCL_HOME", 2, AP_Rally, _rally_incl_home, RALLY_INCLUDE_HOME_DEFAULT),

    // @Param: FS_MODE
    // @DisplayName: 
    // @Description: An alternative mode where RTL will fly to the nearest rally point for a failsafe, but will fly to the takeoff position for normal operations.
    // @User: Standard
    // @Values: 0:DefaultMode,1:RallyOnlyOnFailsafe
    AP_GROUPINFO("FS_MODE", 3, AP_Rally, _rally_fs_mode, 1),

    AP_GROUPEND
};

// constructor
AP_Rally::AP_Rally()
{
#if CONFIG_HAL_BOARD == HAL_BOARD_SITL
    if (_singleton != nullptr) {
        AP_HAL::panic("Rally must be singleton");
    }
#endif
    _singleton = this;
    AP_Param::setup_object_defaults(this, var_info);
}

// get a rally point from EEPROM
bool AP_Rally::get_rally_point_with_index(uint8_t i, RallyLocation &ret) const
{
    if (i >= (uint8_t) _rally_point_total_count) {
        return false;
    }

    _storage.read_block(&ret, i * sizeof(RallyLocation), sizeof(RallyLocation));

    if (ret.lat == 0 && ret.lng == 0) {
        return false; // sanity check
    }

    return true; 
}

void AP_Rally::truncate(uint8_t num)
{
    if (num > _rally_point_total_count) {
        // we never make the space larger this way
        return;
    }
    _rally_point_total_count.set_and_save_ifchanged(num);
}

bool AP_Rally::append(const RallyLocation &loc)
{
    const uint8_t current_total = get_rally_total();
    _rally_point_total_count.set_and_save_ifchanged(current_total + 1);
    if (!set_rally_point_with_index(current_total, loc)) {
        _rally_point_total_count.set_and_save_ifchanged(current_total);
        return false;
    }
    return true;
}

// save a rally point to EEPROM - this assumes that the RALLY_TOTAL param has been incremented beforehand, which is the case in Mission Planner
bool AP_Rally::set_rally_point_with_index(uint8_t i, const RallyLocation &rallyLoc)
{
    if (i >= (uint8_t) _rally_point_total_count) {
        return false;
    }

    if (i >= get_rally_max()) {
        return false;
    }

    _storage.write_block(i * sizeof(RallyLocation), &rallyLoc, sizeof(RallyLocation));

    _last_change_time_ms = AP_HAL::millis();

    AP::logger().Write_RallyPoint(_rally_point_total_count, i, rallyLoc);

    return true;
}

// helper function to translate a RallyLocation to a Location
Location AP_Rally::rally_location_to_location(const RallyLocation &rally_loc) const
{
    Location ret = {};

    // we return an absolute altitude, as we add homeloc.alt below
    ret.relative_alt = false;

    //Currently can't do true AGL on the APM.  Relative altitudes are
    //relative to HOME point's altitude.  Terrain on the board is inbound
    //for the PX4, though.  This line will need to be updated when that happens:
    ret.alt = (rally_loc.alt*100UL) + AP::ahrs().get_home().alt;

    ret.lat = rally_loc.lat;
    ret.lng = rally_loc.lng;

    return ret;
}

// returns true if a valid rally point is found, otherwise returns false to indicate home position should be used
bool AP_Rally::find_nearest_rally_point(const Location &current_loc, RallyLocation &return_loc) const
{
    float min_dis = -1;

    for (uint8_t i = 0; i < (uint8_t) _rally_point_total_count; i++) {
        RallyLocation next_rally;
        if (!get_rally_point_with_index(i, next_rally)) {
            continue;
        }
        Location rally_loc = rally_location_to_location(next_rally);
        float dis = current_loc.get_distance(rally_loc);

        if (is_valid(rally_loc) && (dis < min_dis || min_dis < 0)) {
            min_dis = dis;
            return_loc = next_rally;
        }
    }

    // if a limit is defined and all rally points are beyond that limit, use home if it is closer
    if ((_rally_limit_km > 0) && (min_dis > _rally_limit_km*1000.0f)) {
        return false; // use home position
    }

    // use home if no rally points found
    return min_dis >= 0;
}

Location AP_Rally::calc_best_rally_or_home_location(const Location &current_loc, float rtl_home_alt) const
{
    return calc_best_rally_or_home_location(current_loc, rtl_home_alt, false);
}

// return best RTL location from current position
Location AP_Rally::calc_best_rally_or_home_location(const Location &current_loc, float rtl_home_alt, bool failsafe) const
{
    RallyLocation ral_loc = {};
    Location return_loc = {};
    const struct Location &home_loc = AP::ahrs().get_home();
    
    // no valid rally point, return home position
    return_loc = home_loc;
    return_loc.alt = rtl_home_alt;
    return_loc.relative_alt = false; // read_alt_to_hold returns an absolute altitude


    bool include_home = _rally_incl_home;
    if(_rally_fs_mode>0) 
    {
        if(failsafe) include_home = false; // don't land at home for gcs or radio failsafe (add a parameter as well)
        if(!failsafe)
        {
            // always return to original home position for rally_fs_mode
           return return_loc;
        }
    }

    if (find_nearest_rally_point(current_loc, ral_loc)) {
        Location loc = rally_location_to_location(ral_loc);
        // use the rally point if it's closer then home, or we aren't generally considering home as acceptable
        if (!include_home  || (current_loc.get_distance(loc) < current_loc.get_distance(return_loc))) {
            return_loc = rally_location_to_location(ral_loc);
        }
    }

    return return_loc;
}

// singleton instance
AP_Rally *AP_Rally::_singleton;

namespace AP {

AP_Rally *rally()
{
    return AP_Rally::get_singleton();
}

}
#endif //HAL_RALLY_ENABLED
