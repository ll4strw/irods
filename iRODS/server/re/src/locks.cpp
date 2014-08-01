/* For copyright information please refer to files in the COPYRIGHT directory
 */
#include "debug.hpp"
#include "locks.hpp"
#include "filesystem.hpp"
#include "utils.hpp"
#include "irods_log.hpp"
#include "initServer.hpp"
#include "irods_server_properties.hpp"

int lockMutex( mutex_type **mutex ) {
    std::string mutex_name;
    irods::error ret = getMutexName( mutex_name );
    if ( !ret.ok() ) {
        rodsLog( LOG_ERROR, "lockMutex: call to getMutexName failed" );
        return -1;
    }

    *mutex = new boost::interprocess::named_mutex( boost::interprocess::open_or_create, mutex_name.c_str() );
    ( *mutex )->lock();
    return 0;
}

void unlockMutex( mutex_type **mutex ) {
    ( *mutex )->unlock();
    delete *mutex;
}

/* This function can be used during initialization to remove a previously held mutex that has not been released.
 * This should only be used when there is no other process using the mutex */
void resetMutex( mutex_type **mutex ) {
    std::string mutex_name;
    irods::error ret = getMutexName( mutex_name );
    if ( !ret.ok() ) {
        rodsLog( LOG_ERROR, "resetMutex: call to getMutexName failed" );
    }

    boost::interprocess::named_mutex::remove( mutex_name.c_str() );
}

irods::error getMutexName( std::string &mutex_name ) {
    std::string mutex_name_salt;
    irods::error ret = irods::server_properties::getInstance().get_property<std::string>( RE_CACHE_SALT_KW, mutex_name_salt );
    if ( !ret.ok() ) {
        rodsLog( LOG_ERROR, "getMutexName: failed to retrieve re cache salt from server_properties\n%s", ret.result().c_str() );
        return PASS( ret );
    }

    getResourceName( mutex_name, mutex_name_salt.c_str() );
    mutex_name = "re_cache_mutex_" + mutex_name;

    return SUCCESS();
}
