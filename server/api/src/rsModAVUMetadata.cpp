#include "rsModAVUMetadata.hpp"

#include "modAVUMetadata.h"
#include "icatHighLevelRoutines.hpp"
#include "miscServerFunct.hpp"
#include "irods_configuration_keywords.hpp"
#include "rcConnect.h"
#include "rcMisc.h"
#include "rodsError.h"
#include "rodsErrorTable.h"
#include "irods_logger.hpp"

using logger = irods::experimental::log;

int
rsModAVUMetadata( rsComm_t *rsComm, modAVUMetadataInp_t *modAVUMetadataInp ) {
    rodsServerHost_t *rodsServerHost;
    int status;
    char *myHint;

    if ( strcmp( modAVUMetadataInp->arg0, "add" ) == 0 ) {
        myHint = modAVUMetadataInp->arg2;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "adda" ) == 0 ) {
        myHint = modAVUMetadataInp->arg2;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "addw" ) == 0 ) {
        myHint = modAVUMetadataInp->arg2;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "rmw" ) == 0 ) {
        myHint = modAVUMetadataInp->arg2;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "rmi" ) == 0 ) {
        myHint = modAVUMetadataInp->arg2;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "rm" ) == 0 ) {
        myHint = modAVUMetadataInp->arg2;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "cp" ) == 0 ) {
        myHint = modAVUMetadataInp->arg3;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "mod" ) == 0 ) {
        myHint = modAVUMetadataInp->arg2;
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "set" ) == 0 ) { // JMC - backport 4836
        myHint = modAVUMetadataInp->arg2;
    }
    else {
        /* assume local */
        myHint = NULL;
    }

    status = getAndConnRcatHost( rsComm, MASTER_RCAT, ( const char* )myHint, &rodsServerHost );
    if ( status < 0 ) {
        return status;
    }

    if ( rodsServerHost->localFlag == LOCAL_HOST ) {
        std::string svc_role;
        irods::error ret = get_catalog_service_role(svc_role);
        if(!ret.ok()) {
            irods::log(PASS(ret));
            return ret.code();
        }

        if( irods::CFG_SERVICE_ROLE_PROVIDER == svc_role ) {
            status = _rsModAVUMetadata( rsComm, modAVUMetadataInp );
        } else if( irods::CFG_SERVICE_ROLE_CONSUMER == svc_role ) {
            status = SYS_NO_RCAT_SERVER_ERR;
        } else {
            rodsLog(
                LOG_ERROR,
                "role not supported [%s]",
                svc_role.c_str() );
            status = SYS_SERVICE_ROLE_NOT_SUPPORTED;
        }

    }
    else {
        status = rcModAVUMetadata( rodsServerHost->conn,
                                   modAVUMetadataInp );
    }

    if ( status < 0 ) {
        rodsLog( LOG_NOTICE,
                 "rsModAVUMetadata: rcModAVUMetadata failed" );
    }
    return status;
}

int
_rsModAVUMetadata( rsComm_t *rsComm, modAVUMetadataInp_t *modAVUMetadataInp ) {
    int status, status2;

    const char *args[MAX_NUM_OF_ARGS_IN_ACTION];
    int argc;
    ruleExecInfo_t rei2;

    memset( ( char* )&rei2, 0, sizeof( ruleExecInfo_t ) );
    rei2.rsComm = rsComm;
    if ( rsComm != NULL ) {
        rei2.uoic = &rsComm->clientUser;
        rei2.uoip = &rsComm->proxyUser;
    }

    args[0] = modAVUMetadataInp->arg0; // option add, adda, rm, rmw, rmi, cp, mod, or set
    args[1] = modAVUMetadataInp->arg1; // item type -d,-D,-c,-C,-r,-R,-u,-U
    args[2] = modAVUMetadataInp->arg2; // item name
    args[3] = modAVUMetadataInp->arg3; // attr name
    args[4] = modAVUMetadataInp->arg4; // attr val
    args[5] = modAVUMetadataInp->arg5; // attr unit
    if ( args[5] == NULL ) { args[5] = ""; }
    if ( strcmp( args[0], "mod" ) == 0 ) {
        argc = 9;
#define ARG(arg) { int ix; if( ( ix = checkModArgType(arg) ) >= 0 ) avu[ix] = arg; }
        if ( checkModArgType( modAVUMetadataInp->arg5 ) != 0 ) {
            char *avu[4] = {"", "", "", ""};
            ARG( modAVUMetadataInp->arg5 );
            ARG( modAVUMetadataInp->arg6 );
            ARG( modAVUMetadataInp->arg7 );
            args[5] = "";
            memcpy( args + 6, avu + 1, sizeof( char *[3] ) );
        }
        else {
            char *avu[4] = {"", "", "", ""};
            ARG( modAVUMetadataInp->arg6 ); /* new attr */
            ARG( modAVUMetadataInp->arg7 ); /* new val */
            ARG( modAVUMetadataInp->arg8 ); /* new unit */
            memcpy( args + 6, avu + 1, sizeof( char *[3] ) );
        }
    }
    else if ( strcmp( args[0], "cp" ) == 0 ) {
        argc = 5;
    }
    else {
        argc = 6;
    }

    status2 =  applyRuleArg( "acPreProcForModifyAVUMetadata", args, argc,
                             &rei2, NO_SAVE_REI );
    if ( status2 < 0 ) {
        if ( rei2.status < 0 ) {
            status2 = rei2.status;
        }
        rodsLog( LOG_ERROR,
                 "rsModAVUMetadata:acPreProcForModifyAVUMetadata error for %s of type %s and option %s,stat=%d",
                 modAVUMetadataInp->arg2, modAVUMetadataInp->arg1, modAVUMetadataInp->arg0, status2 );
        return status2;
    }

    if ( strcmp( modAVUMetadataInp->arg0, "add" ) == 0 ) {
        status = chlAddAVUMetadata(rsComm,
                                   modAVUMetadataInp->arg1,
                                   modAVUMetadataInp->arg2,
                                   modAVUMetadataInp->arg3,
                                   modAVUMetadataInp->arg4,
                                   modAVUMetadataInp->arg5,
                                   &modAVUMetadataInp->condInput);
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "adda" ) == 0 ) {
        const char* const msg = R"_("adda" is deprecated. Please use "add" with admin mode enabled instead.)_";
        logger::api::debug("{} - {}", __func__, msg);
        addRErrorMsg(&rsComm->rError, DEPRECATED_PARAMETER, msg);

        // Determine if the client set the ADMIN_KW.
        // If they haven't, set it and remember to remove it once the function returns.
        // This keeps the key value pair's state clean.
        //
        // Setting the ADMIN_KW is required for proper operation of the "adda" subcommand.
        const bool remove_admin_keyword = !getValByKey(&modAVUMetadataInp->condInput, ADMIN_KW);

        if (remove_admin_keyword) {
            addKeyVal(&modAVUMetadataInp->condInput, ADMIN_KW, "");
        }

        status = chlAddAVUMetadata(rsComm,
                                   modAVUMetadataInp->arg1,
                                   modAVUMetadataInp->arg2,
                                   modAVUMetadataInp->arg3,
                                   modAVUMetadataInp->arg4,
                                   modAVUMetadataInp->arg5,
                                   &modAVUMetadataInp->condInput);

        if (remove_admin_keyword) {
            // Restore the key value pair to its original state.
            rmKeyVal(&modAVUMetadataInp->condInput, ADMIN_KW);
        }
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "addw" ) == 0 ) {
        status = chlAddAVUMetadataWild(rsComm,
                                       modAVUMetadataInp->arg1,
                                       modAVUMetadataInp->arg2,
                                       modAVUMetadataInp->arg3,
                                       modAVUMetadataInp->arg4,
                                       modAVUMetadataInp->arg5,
                                       &modAVUMetadataInp->condInput);
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "rmw" ) == 0 ) {
        status = chlDeleteAVUMetadata(rsComm, 1,
                                      modAVUMetadataInp->arg1,
                                      modAVUMetadataInp->arg2,
                                      modAVUMetadataInp->arg3,
                                      modAVUMetadataInp->arg4,
                                      modAVUMetadataInp->arg5,
                                      0,
                                      &modAVUMetadataInp->condInput);
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "rmi" ) == 0 ) {
        status = chlDeleteAVUMetadata(rsComm, 2,
                                      modAVUMetadataInp->arg1,
                                      modAVUMetadataInp->arg2,
                                      modAVUMetadataInp->arg3,
                                      modAVUMetadataInp->arg4,
                                      modAVUMetadataInp->arg5,
                                      0,
                                      &modAVUMetadataInp->condInput);
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "rm" ) == 0 ) {
        status = chlDeleteAVUMetadata(rsComm, 0,
                                      modAVUMetadataInp->arg1,
                                      modAVUMetadataInp->arg2,
                                      modAVUMetadataInp->arg3,
                                      modAVUMetadataInp->arg4,
                                      modAVUMetadataInp->arg5,
                                      0,
                                      &modAVUMetadataInp->condInput);
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "cp" ) == 0 ) {
        status = chlCopyAVUMetadata(rsComm,
                                    modAVUMetadataInp->arg1,
                                    modAVUMetadataInp->arg2,
                                    modAVUMetadataInp->arg3,
                                    modAVUMetadataInp->arg4,
                                    &modAVUMetadataInp->condInput);
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "mod" ) == 0 ) {
        status = chlModAVUMetadata(rsComm,
                                   modAVUMetadataInp->arg1,
                                   modAVUMetadataInp->arg2,
                                   modAVUMetadataInp->arg3,
                                   modAVUMetadataInp->arg4,
                                   modAVUMetadataInp->arg5,
                                   modAVUMetadataInp->arg6,
                                   modAVUMetadataInp->arg7,
                                   modAVUMetadataInp->arg8,
                                   &modAVUMetadataInp->condInput);
    }
    else if ( strcmp( modAVUMetadataInp->arg0, "set" ) == 0 ) { // JMC - backport 4836
        status = chlSetAVUMetadata(rsComm,
                                   modAVUMetadataInp->arg1,
                                   modAVUMetadataInp->arg2,
                                   modAVUMetadataInp->arg3,
                                   modAVUMetadataInp->arg4,
                                   modAVUMetadataInp->arg5,
                                   &modAVUMetadataInp->condInput);
    }
    else {
        return CAT_INVALID_ARGUMENT;
    }

    if ( status >= 0 ) {
        status2 =  applyRuleArg( "acPostProcForModifyAVUMetadata", args, argc, &rei2, NO_SAVE_REI );
        if ( status2 < 0 ) {
            if ( rei2.status < 0 ) {
                status2 = rei2.status;
            }
            rodsLog( LOG_ERROR,
                     "rsModAVUMetadata:acPostProcForModifyAVUMetadata error for %s of type %s and option %s,stat=%d",
                     modAVUMetadataInp->arg2,
                     modAVUMetadataInp->arg1,
                     modAVUMetadataInp->arg0,
                     status2 );
            return status2;
        }
    }

    return status;

}
