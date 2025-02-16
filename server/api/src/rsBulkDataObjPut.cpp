#include "irods/rsBulkDataObjPut.hpp"

#include "irods/apiHeaderAll.h"
#include "irods/objMetaOpr.hpp"
#include "irods/resource.hpp"
#include "irods/collection.hpp"
#include "irods/specColl.hpp"
#include "irods/dataObjOpr.hpp"
#include "irods/physPath.hpp"
#include "irods/miscServerFunct.hpp"
#include "irods/rcGlobalExtern.h"
#include "irods/rsApiHandler.hpp"
#include "irods/irods_stacktrace.hpp"
#include "irods/checksum.h"
#include "irods/rsDataObjPut.hpp"
#include "irods/rsStructFileExtAndReg.hpp"
#include "irods/rsBulkDataObjReg.hpp"
#include "irods/rsDataObjCreate.hpp"
#include "irods/irods_logger.hpp"
#include "irods/irods_server_properties.hpp"
#include "irods/irods_resource_backport.hpp"
#include "irods/irods_resource_redirect.hpp"
#include "irods/irods_random.hpp"
#include "irods/key_value_proxy.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/lexical_cast.hpp>

#include <fmt/format.h>

#include <cstring>

namespace fs = boost::filesystem;
using log_api = irods::experimental::log::api;

int
_rsBulkDataObjPut( rsComm_t *rsComm, bulkOprInp_t *bulkOprInp,
                   bytesBuf_t *bulkOprInpBBuf );
int
createBunDirForBulkPut( rsComm_t *rsComm, dataObjInp_t *dataObjInp,
                        const char*, specColl_t *specColl, char *phyBunDir );
int
initDataObjInpFromBulkOpr( dataObjInp_t *dataObjInp, bulkOprInp_t *bulkOprInp );
int
bulkRegUnbunSubfiles( rsComm_t *rsComm, const char*, const std::string& rescHier,
                      char *collection, char *phyBunDir, int flags,
                      genQueryOut_t *attriArray );
int
_bulkRegUnbunSubfiles( rsComm_t *rsComm, const char*, const std::string& rescHier,
                       char *collection, char *phyBunDir, int flags,
                       genQueryOut_t *bulkDataObjRegInp, renamedPhyFiles_t *renamedPhyFiles,
                       genQueryOut_t *attriArray );
int
bulkProcAndRegSubfile( rsComm_t *rsComm, const char*, const std::string& rescHier,
                       char *subObjPath, char *subfilePath, rodsLong_t dataSize,
                       int dataMode, int flags, genQueryOut_t *bulkDataObjRegInp,
                       renamedPhyFiles_t *renamedPhyFiles, genQueryOut_t *attriArray );
int
bulkRegSubfile( rsComm_t *rsComm, const char*, const std::string& rescHier,
                char *subObjPath, char *subfilePath, rodsLong_t dataSize, int dataMode,
                int modFlag, int replNum, char *chksum, genQueryOut_t *bulkDataObjRegInp,
                renamedPhyFiles_t *renamedPhyFiles );

int
addRenamedPhyFile(
    char *subObjPath,
    char *oldFileName,
    char *newFileName,
    renamedPhyFiles_t *renamedPhyFiles );

int
cleanupBulkRegFiles(
    rsComm_t *rsComm,
    genQueryOut_t *bulkDataObjRegInp );

int
postProcBulkPut(
    rsComm_t *rsComm,
    genQueryOut_t *bulkDataObjRegInp,
    genQueryOut_t *bulkDataObjRegOut );

int
postProcRenamedPhyFiles(
    renamedPhyFiles_t *renamedPhyFiles,
    int regStatus );

int
rsBulkDataObjPut( rsComm_t *rsComm, bulkOprInp_t *bulkOprInp,
                  bytesBuf_t *bulkOprInpBBuf ) {
    int status;
    int remoteFlag;
    rodsServerHost_t *rodsServerHost;
    specCollCache_t *specCollCache = NULL;
    dataObjInp_t dataObjInp;

    resolveLinkedPath( rsComm, bulkOprInp->objPath, &specCollCache,
                       &bulkOprInp->condInput );

    /* need to setup dataObjInp */
    initDataObjInpFromBulkOpr( &dataObjInp, bulkOprInp );

    remoteFlag = getAndConnRemoteZone( rsComm, &dataObjInp, &rodsServerHost,
                                       REMOTE_CREATE );

    if ( remoteFlag < 0 ) {
        return remoteFlag;
    }
    else if ( remoteFlag == LOCAL_HOST ) {
        int               local = LOCAL_HOST;
        rodsServerHost_t* host  =  0;
        if ( getValByKey( &dataObjInp.condInput, RESC_HIER_STR_KW ) == NULL ) {
            std::string       hier;
            irods::error ret = irods::resource_redirect( irods::CREATE_OPERATION, rsComm,
                               &dataObjInp, hier, host, local );
            if ( !ret.ok() ) {
                std::stringstream msg;
                msg << "failed for [";
                msg << dataObjInp.objPath << "]";
                irods::log( PASSMSG( msg.str(), ret ) );
                return ret.code();
            }

            // =-=-=-=-=-=-=-
            // we resolved the redirect and have a host, set the hier str for subsequent
            // api calls, etc.
            addKeyVal( &bulkOprInp->condInput, RESC_HIER_STR_KW, hier.c_str() );

        } // if keyword

        if ( LOCAL_HOST == local ) {
            status = _rsBulkDataObjPut( rsComm, bulkOprInp, bulkOprInpBBuf );
        }
        else {
            status = rcBulkDataObjPut( host->conn, bulkOprInp, bulkOprInpBBuf );

        }
    }
    else {
        status = rcBulkDataObjPut( rodsServerHost->conn, bulkOprInp,
                                   bulkOprInpBBuf );
    }
    return status;
}

int
unbunBulkBuf(rsComm_t* rsComm,
             dataObjInp_t* dataObjInp,
             bulkOprInp_t* bulkOprInp,
             bytesBuf_t* bulkBBuf)
{
    if (!bulkOprInp) {
        return USER__NULL_INPUT_ERR;
    }

    genQueryOut_t* attriArray = &bulkOprInp->attriArray;

    sqlResult_t* objPath;
    if (!(objPath = getSqlResultByInx( attriArray, COL_DATA_NAME))) {
        log_api::info("unbunBulkBuf: getSqlResultByInx for COL_DATA_NAME failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    sqlResult_t* offset;
    if (!(offset = getSqlResultByInx( attriArray, OFFSET_INX))) {
        log_api::info("unbunBulkBuf: getSqlResultByInx for OFFSET_INX failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    irods::experimental::key_value_proxy kvp{dataObjInp->condInput};

    sqlResult_t* checksum{};
    if (kvp.contains(VERIFY_CHKSUM_KW)) {
        if (!(checksum = getSqlResultByInx(attriArray, COL_D_DATA_CHECKSUM))) {
            log_api::info("unbunBulkBuf: getSqlResultByInx for COL_D_DATA_CHECKSUM failed");
            return UNMATCHED_KEY_OR_INDEX;
        }
    }

    if (attriArray->rowCnt > MAX_NUM_BULK_OPR_FILES) {
        log_api::info("unbunBulkBuf: rowCnt {} too large", attriArray->rowCnt);
        return SYS_REQUESTED_BUF_TOO_LARGE;
    }

    int intOffset[MAX_NUM_BULK_OPR_FILES];

    for (int i = 0; i < attriArray->rowCnt; i++) {
        intOffset[i] = atoi(&offset->value[offset->len * i]);
    }

    kvp[DATA_INCLUDED_KW] = "";

    for (int i = 0; i < attriArray->rowCnt; ++i) {
        bytesBuf_t buffer;
        if (0 == i) {
            buffer.buf = static_cast<char*>(bulkBBuf->buf);
            buffer.len = intOffset[0];
        }
        else {
            buffer.buf = static_cast<char*>(bulkBBuf->buf) + intOffset[i - 1];
            buffer.len = intOffset[i] - intOffset[i - 1];
        }

        char* tmpObjPath = &objPath->value[objPath->len * i];
        std::string collString = tmpObjPath;
        std::size_t last_slash = collString.find_last_of('/');
        collString.erase(last_slash);

        if (const auto ec = rsMkCollR(rsComm, "/", collString.c_str()); ec < 0) {
            irods::log(LOG_ERROR, fmt::format("{}: Unable to make collection [{}].", __FUNCTION__, collString));
            return ec;
        }

        rstrcpy(dataObjInp->objPath, tmpObjPath, MAX_NAME_LEN);

        if (checksum) {
            kvp[VERIFY_CHKSUM_KW] = &checksum->value[checksum->len * i];
        }

        if (const auto ec = rsDataObjPut(rsComm, dataObjInp, &buffer, nullptr); ec < 0) {
            irods::log(LOG_ERROR, fmt::format("{}: Failed to put data into file [{}].", __FUNCTION__, tmpObjPath));
            return ec;
        }
    }

    return 0;
}

int
_rsBulkDataObjPut( rsComm_t *rsComm, bulkOprInp_t *bulkOprInp,
                   bytesBuf_t *bulkOprInpBBuf ) {
    int status;
    char phyBunDir[MAX_NAME_LEN];
    std::string resc_name;
    dataObjInp_t dataObjInp;
    rodsObjStat_t *myRodsObjStat = NULL;


    status = chkCollForExtAndReg( rsComm, bulkOprInp->objPath, &myRodsObjStat );
    if ( status < 0 || myRodsObjStat == NULL ) {
        return status; // JMC cppcheck
    }

    /* query rcat for resource info and sort it */

    /* need to setup dataObjInp */
    initDataObjInpFromBulkOpr( &dataObjInp, bulkOprInp );

    if ( myRodsObjStat->specColl != NULL ) {
        resc_name = myRodsObjStat->specColl->resource;
    }
    else {
        const auto resc_hier = getValByKey(&dataObjInp.condInput, RESC_HIER_STR_KW);
        if (!resc_hier) {
            freeRodsObjStat( myRodsObjStat );
            return SYS_INTERNAL_NULL_INPUT_ERR;
        }
        resc_name = irods::hierarchy_parser{resc_hier}.first_resc();
        if (resc_name.empty()) {
            freeRodsObjStat( myRodsObjStat );
            return status;
        }
    }

    status = createBunDirForBulkPut( rsComm, &dataObjInp, resc_name.c_str(), myRodsObjStat->specColl, phyBunDir );
    if ( status < 0 ) {
        freeRodsObjStat( myRodsObjStat );

        std::stringstream msg;
        msg << __FUNCTION__ << ": Unable to create BunDir";
        irods::log( LOG_ERROR, msg.str() );
        return status;
    }

    status = rsMkCollR( rsComm, "/", bulkOprInp->objPath );
    if ( status < 0 ) {
        freeRodsObjStat( myRodsObjStat );

        std::stringstream msg;
        msg << __FUNCTION__ << ": Unable to make collection \"" << bulkOprInp->objPath << "\"";
        irods::log( LOG_ERROR, msg.str() );
        return status;
    }

    // addKeyVal(&dataObjInp.condInput, FORCE_FLAG_KW, getValByKey (&bulkOprInp->condInput, FORCE_FLAG_KW));
    // addKeyVal(&dataObjInp.condInput, VERIFY_CHKSUM_KW, getValByKey (&bulkOprInp->condInput, VERIFY_CHKSUM_KW));

    status = unbunBulkBuf( rsComm, &dataObjInp, bulkOprInp, bulkOprInpBBuf );

    freeRodsObjStat( myRodsObjStat );

    if ( status < 0 ) {
        log_api::error("_rsBulkDataObjPut: unbunBulkBuf for dir {}. stat = {}", phyBunDir, status);
        return status;
    }

    return status;
}

int
createBunDirForBulkPut( rsComm_t *rsComm, dataObjInp_t *dataObjInp,
                        const char *_resc_name, specColl_t *specColl, char *phyBunDir ) {
    dataObjInfo_t dataObjInfo;
    int status;

    if ( !dataObjInp || !_resc_name || !phyBunDir ) {
        return USER__NULL_INPUT_ERR;
    }

    std::memset(&dataObjInfo, 0, sizeof( dataObjInfo));
    rstrcpy( dataObjInfo.objPath, dataObjInp->objPath, MAX_NAME_LEN );
    rstrcpy( dataObjInfo.rescName, _resc_name, NAME_LEN );

    char* resc_hier = getValByKey( &dataObjInp->condInput, RESC_HIER_STR_KW );
    if ( resc_hier ) {
        rstrcpy( dataObjInfo.rescHier, resc_hier, MAX_NAME_LEN );
    }
    else {
        rstrcpy( dataObjInfo.rescHier, _resc_name, NAME_LEN ); // in kw else
    }

    irods::error ret = resc_mgr.hier_to_leaf_id(resc_hier,dataObjInfo.rescId);
    if( !ret.ok() ) {
        irods::log(PASS(ret));
    }

    if ( specColl != NULL ) {
        status = getMountedSubPhyPath( specColl->collection,
                                       specColl->phyPath, dataObjInp->objPath, phyBunDir );
        if ( status < 0 ) {
            return status;
        }
        status = mkFileDirR( rsComm, 0, phyBunDir, dataObjInfo.rescHier, getDefDirMode() );
        if ( status < 0 ) {
            log_api::error("mkFileDirR failed in createBunDirForBulkPut with status {}", status);
        }
        return status;
    }

    status = getFilePathName( rsComm, &dataObjInfo, dataObjInp );
    if ( status < 0 ) {
        log_api::error("createBunDirForBulkPut: getFilePathName err for {}. status = {}", dataObjInp->objPath, status);
        return status;
    }
    do {
        snprintf( phyBunDir, MAX_NAME_LEN, "%s/%s.%u", dataObjInfo.filePath,
                  TMP_PHY_BUN_DIR, irods::getRandom<unsigned int>() );
        fs::path p( phyBunDir );
        if ( exists( p ) ) {
            status = 0;
        }
        else {
            status = -1;
        }
    }
    while ( status == 0 );

    status = mkFileDirR( rsComm, 0, phyBunDir, dataObjInfo.rescHier, getDefDirMode() );
    if ( status < 0 ) {
        log_api::error("mkFileDirR failed in createBunDirForBulkPut with status {}", status);
    }
    return status;

}

int
initDataObjInpFromBulkOpr( dataObjInp_t *dataObjInp, bulkOprInp_t *bulkOprInp ) {
    if ( dataObjInp == NULL || bulkOprInp == NULL ) {
        return USER__NULL_INPUT_ERR;
    }

    std::memset(dataObjInp, 0, sizeof(dataObjInp_t));
    rstrcpy( dataObjInp->objPath, bulkOprInp->objPath, MAX_NAME_LEN );
    dataObjInp->condInput = bulkOprInp->condInput;

    return 0;
}

int
bulkRegUnbunSubfiles( rsComm_t *rsComm, const char *_resc_name, const std::string& rescHier,
                      char *collection, char *phyBunDir, int flags,
                      genQueryOut_t *attriArray ) {
    genQueryOut_t bulkDataObjRegInp;
    renamedPhyFiles_t renamedPhyFiles;
    int status = 0;

    std::memset(&renamedPhyFiles, 0, sizeof(renamedPhyFiles));
    initBulkDataObjRegInp( &bulkDataObjRegInp );
    /* the continueInx is used for the matching of objPath */
    if ( attriArray != NULL ) {
        attriArray->continueInx = 0;
    }

    status = _bulkRegUnbunSubfiles( rsComm, _resc_name, rescHier, collection,
                                    phyBunDir, flags, &bulkDataObjRegInp, &renamedPhyFiles, attriArray );

    if ( bulkDataObjRegInp.rowCnt > 0 ) {
        int status1;
        genQueryOut_t *bulkDataObjRegOut = NULL;
        status1 = rsBulkDataObjReg( rsComm, &bulkDataObjRegInp,
                                    &bulkDataObjRegOut );
        if ( status1 < 0 ) {
            status = status1;
            log_api::error("{}: rsBulkDataObjReg error for {}. stat = {}", __FUNCTION__, collection, status1);
            cleanupBulkRegFiles( rsComm, &bulkDataObjRegInp );
        }
        postProcRenamedPhyFiles( &renamedPhyFiles, status );
        postProcBulkPut( rsComm, &bulkDataObjRegInp, bulkDataObjRegOut );
        freeGenQueryOut( &bulkDataObjRegOut );
    }
    clearGenQueryOut( &bulkDataObjRegInp );
    return status;
}

int
_bulkRegUnbunSubfiles( rsComm_t *rsComm, const char *_resc_name, const std::string& rescHier,
                       char *collection, char *phyBunDir, int flags,
                       genQueryOut_t *bulkDataObjRegInp, renamedPhyFiles_t *renamedPhyFiles,
                       genQueryOut_t *attriArray ) {
    char subfilePath[MAX_NAME_LEN];
    char subObjPath[MAX_NAME_LEN];
    dataObjInp_t dataObjInp;
    int status;
    int savedStatus = 0;
    int st_mode;
    rodsLong_t st_size;

    fs::path srcDirPath( phyBunDir );
    if ( !exists( srcDirPath ) || !is_directory( srcDirPath ) ) {
        log_api::error("{}: opendir error for {}, errno = {}", __FUNCTION__, phyBunDir, errno);
        return UNIX_FILE_OPENDIR_ERR - errno;
    }

    std::memset(&dataObjInp, 0, sizeof(dataObjInp));
    fs::directory_iterator end_itr; // default construction yields past-the-end
    for ( fs::directory_iterator itr( srcDirPath ); itr != end_itr; ++itr ) {
        fs::path p = itr->path();
        snprintf( subfilePath, MAX_NAME_LEN, "%s",
                  p.c_str() );

        if ( !exists( p ) ) {
            log_api::error("{}: stat error for {}, errno = {}", __FUNCTION__, subfilePath, errno);
            savedStatus = UNIX_FILE_STAT_ERR - errno;
            unlink( subfilePath );
            continue;
        }

        fs::path childPath = p.filename();
        snprintf( subObjPath, MAX_NAME_LEN, "%s/%s",
                  collection, childPath.c_str() );
        if ( is_directory( p ) ) {
            status = rsMkCollR( rsComm, "/", subObjPath );
            if ( status < 0 ) {
                log_api::error("{}: rsMkCollR of {} error. status = {}", __FUNCTION__, subObjPath, status);
                savedStatus = status;
                continue;
            }
            status = _bulkRegUnbunSubfiles( rsComm, _resc_name, rescHier,
                                            subObjPath, subfilePath, flags, bulkDataObjRegInp,
                                            renamedPhyFiles, attriArray );
            if ( status < 0 ) {
                log_api::error("{}: _bulkRegUnbunSubfiles of {} error. status={}", __FUNCTION__, subObjPath, status);
                savedStatus = status;
                continue;
            }
        }
        else if ( is_regular_file( p ) ) {
            st_mode = getPathStMode( p.c_str() );
            st_size = file_size( p );
            status = bulkProcAndRegSubfile( rsComm, _resc_name, rescHier,
                                            subObjPath, subfilePath, st_size,
                                            st_mode & 0777, flags, bulkDataObjRegInp,
                                            renamedPhyFiles, attriArray );
            unlink( subfilePath );
            if ( status < 0 ) {
                log_api::error("{}:bulkProcAndRegSubfile of {} err.stat={}", __FUNCTION__, subObjPath, status);
                savedStatus = status;
                continue;
            }
        }
    }
    rmdir( phyBunDir );
    return savedStatus;
}

int
bulkProcAndRegSubfile( rsComm_t *rsComm, const char *_resc_name, const std::string& rescHier,
                       char *subObjPath, char *subfilePath, rodsLong_t dataSize,
                       int dataMode, int flags, genQueryOut_t *bulkDataObjRegInp,
                       renamedPhyFiles_t *renamedPhyFiles, genQueryOut_t *attriArray ) {
    dataObjInfo_t dataObjInfo;
    dataObjInp_t dataObjInp;
    int status;
    int modFlag = 0;
    char *myChksum = NULL;
    int myDataMode = dataMode;

    std::memset(&dataObjInp, 0, sizeof(dataObjInp));
    std::memset(&dataObjInfo, 0, sizeof(dataObjInfo));
    rstrcpy( dataObjInp.objPath, subObjPath, MAX_NAME_LEN );
    rstrcpy( dataObjInfo.objPath, subObjPath, MAX_NAME_LEN );
    rstrcpy( dataObjInfo.rescName, _resc_name, NAME_LEN );
    rstrcpy( dataObjInfo.rescHier, rescHier.c_str(), MAX_NAME_LEN );
    rstrcpy( dataObjInfo.dataType, "generic", NAME_LEN );
    dataObjInfo.dataSize = dataSize;

    irods::error ret = resc_mgr.hier_to_leaf_id(rescHier,dataObjInfo.rescId);
    if( !ret.ok() ) {
        irods::log(PASS(ret));
    }

    status = getFilePathName( rsComm, &dataObjInfo, &dataObjInp );
    if ( status < 0 ) {
        log_api::error("regSubFile: getFilePathName err for {}. status = {}", dataObjInp.objPath, status);
        return status;
    }

    fs::path p( dataObjInfo.filePath );
    if ( exists( p ) ) {
        if ( is_directory( p ) ) {
            return SYS_PATH_IS_NOT_A_FILE;
        }
        if ( chkOrphanFile( rsComm, dataObjInfo.filePath, _resc_name,
                            &dataObjInfo ) <= 0 ) {
            /* not an orphan file */
            if ( ( flags & FORCE_FLAG_FLAG ) != 0 && dataObjInfo.dataId > 0 &&
                    strcmp( dataObjInfo.objPath, subObjPath ) == 0 ) {
                /* overwrite the current file */
                modFlag = 1;
            }
            else {
                status = SYS_COPY_ALREADY_IN_RESC;
                log_api::error(
                    "bulkProcAndRegSubfile: phypath {} is already in use. status = {}", dataObjInfo.filePath, status);
                return status;
            }
        }
        /* rename it to the orphan dir */
        fileRenameInp_t fileRenameInp;
        std::memset(&fileRenameInp, 0, sizeof(fileRenameInp));
        rstrcpy( fileRenameInp.oldFileName, dataObjInfo.filePath, MAX_NAME_LEN );
        rstrcpy( fileRenameInp.rescHier, dataObjInfo.rescHier, MAX_NAME_LEN );
        char new_fn[ MAX_NAME_LEN ];
        status = renameFilePathToNewDir( rsComm, ORPHAN_DIR,
                                         &fileRenameInp, 1, new_fn );
        if ( status < 0 ) {
            log_api::error(
                "bulkProcAndRegSubfile: renameFilePathToNewDir err for {}. status = {}",
                fileRenameInp.oldFileName,
                status);
            return status;
        }
        if ( modFlag > 0 ) {
            status = addRenamedPhyFile( subObjPath, fileRenameInp.oldFileName,
                                        fileRenameInp.newFileName, renamedPhyFiles );
            if ( status < 0 ) {
                return status;
            }
        }
    }
    else {
        /* make the necessary dir */
        status = mkDirForFilePath(
                     rsComm,
                     0,
                     dataObjInfo.filePath,
                     dataObjInfo.rescHier,
                     getDefDirMode() );
        if ( status < 0 ) {
            log_api::error("mkDirForFilePath failed in bulkProcAndRegSubfile with status {}", status);
            return status;
        }
    }
    /* add a link */
#ifndef windows_platform
    status = link( subfilePath, dataObjInfo.filePath );
    if ( status < 0 ) {
        log_api::error(
            "bulkProcAndRegSubfile: link error {} to {}. errno = {}", subfilePath, dataObjInfo.filePath, errno);
        return UNIX_FILE_LINK_ERR - errno;
    }
#endif


    if ( attriArray != NULL ) {
        /* dataMode in attriArray overwrites passed in value */
        status = getAttriInAttriArray( subObjPath, attriArray, &myDataMode,
                                       &myChksum );
        if ( status < 0 ) {
            log_api::info("bulkProcAndRegSubfile: matchObjPath error for {}, stat = {}", subObjPath, status);
        }
        else {
            if ( ( flags & VERIFY_CHKSUM_FLAG ) != 0 && myChksum != NULL ) {
                char chksumStr[NAME_LEN];
                /* verify the chksum */
                status = verifyChksumLocFile( dataObjInfo.filePath, myChksum, chksumStr );
                if ( status < 0 ) {
                    log_api::error("bulkProcAndRegSubfile: chksumLocFile error for {} ", dataObjInfo.filePath);
                    return status;
                }
                if ( strcmp( myChksum, chksumStr ) != 0 ) {
                    log_api::error(
                        "bulkProcAndRegSubfile: chksum of {} {} != input {}",
                        dataObjInfo.filePath,
                        chksumStr,
                        myChksum);
                    return USER_CHKSUM_MISMATCH;
                }
            }
        }
    }

    status = bulkRegSubfile( rsComm, _resc_name, rescHier,
                             subObjPath, dataObjInfo.filePath, dataSize, myDataMode, modFlag,
                             dataObjInfo.replNum, myChksum, bulkDataObjRegInp, renamedPhyFiles );

    return status;
}

int
fillBulkDataObjRegInp( const char * rescName, const char* rescHier, char * objPath,
                       char * filePath, char * dataType, rodsLong_t dataSize, int dataMode,
                       int modFlag, int replNum, char * chksum, genQueryOut_t * bulkDataObjRegInp ) {

    int rowCnt;

    if ( bulkDataObjRegInp == NULL || rescName == NULL || objPath == NULL ||
            filePath == NULL ) {
        return USER__NULL_INPUT_ERR;
    }

    rowCnt = bulkDataObjRegInp->rowCnt;

    if ( rowCnt >= MAX_NUM_BULK_OPR_FILES ) {
        return SYS_BULK_REG_COUNT_EXCEEDED;
    }

    rstrcpy( &bulkDataObjRegInp->sqlResult[0].value[MAX_NAME_LEN * rowCnt],
             objPath, MAX_NAME_LEN );
    rstrcpy( &bulkDataObjRegInp->sqlResult[1].value[NAME_LEN * rowCnt],
             dataType, NAME_LEN );
    snprintf( &bulkDataObjRegInp->sqlResult[2].value[NAME_LEN * rowCnt],
              NAME_LEN, "%lld", dataSize );
    rstrcpy( &bulkDataObjRegInp->sqlResult[3].value[NAME_LEN * rowCnt],
             rescName, NAME_LEN );
    rstrcpy( &bulkDataObjRegInp->sqlResult[4].value[MAX_NAME_LEN * rowCnt],
             filePath, MAX_NAME_LEN );
    snprintf( &bulkDataObjRegInp->sqlResult[5].value[NAME_LEN * rowCnt],
              NAME_LEN, "%d", dataMode );
    if ( modFlag == 1 ) {
        rstrcpy( &bulkDataObjRegInp->sqlResult[6].value[NAME_LEN * rowCnt],
                 MODIFY_OPR, NAME_LEN );
    }
    else {
        rstrcpy( &bulkDataObjRegInp->sqlResult[6].value[NAME_LEN * rowCnt],
                 REGISTER_OPR, NAME_LEN );
    }
    snprintf( &bulkDataObjRegInp->sqlResult[7].value[NAME_LEN * rowCnt],
              NAME_LEN, "%d", replNum );
    if ( chksum != NULL && strlen( chksum ) > 0 ) {
        rstrcpy( &bulkDataObjRegInp->sqlResult[8].value[NAME_LEN * rowCnt],
                 chksum, NAME_LEN );
    }
    else {
        bulkDataObjRegInp->sqlResult[8].value[NAME_LEN * rowCnt] = '\0';
    }

    rodsLong_t resc_id = 0;
    irods::error ret = resc_mgr.hier_to_leaf_id(rescHier,resc_id);
    if(!ret.ok()) {
        irods::log(PASS(ret));
        return ret.code();
    }

    try {
        std::string resc_id_str = boost::lexical_cast<std::string>(resc_id);
        snprintf( &bulkDataObjRegInp->sqlResult[9].value[MAX_NAME_LEN * rowCnt],
                  MAX_NAME_LEN, "%s", resc_id_str.c_str() );
    }
    catch( boost::bad_lexical_cast& ) {
        log_api::error("failed to cast [{}d] to a string", resc_id);
        return SYS_INVALID_INPUT_PARAM;
    }

    bulkDataObjRegInp->rowCnt++;

    return 0;
}

int
bulkRegSubfile( rsComm_t *rsComm, const char *rescName, const std::string& rescHier,
                char *subObjPath, char *subfilePath, rodsLong_t dataSize, int dataMode,
                int modFlag, int replNum, char *chksum, genQueryOut_t *bulkDataObjRegInp,
                renamedPhyFiles_t *renamedPhyFiles ) {
    int status;

    /* XXXXXXXX use NULL for chksum for now */
    status = fillBulkDataObjRegInp( rescName, rescHier.c_str(), subObjPath,
                                    subfilePath, "generic", dataSize, dataMode, modFlag,
                                    replNum, chksum, bulkDataObjRegInp );
    if ( status < 0 ) {
        log_api::error("bulkRegSubfile: fillBulkDataObjRegInp error for {}. status = {}", subfilePath, status);
        return status;
    }

    if ( bulkDataObjRegInp->rowCnt >= MAX_NUM_BULK_OPR_FILES ) {
        genQueryOut_t *bulkDataObjRegOut = NULL;
        status = rsBulkDataObjReg( rsComm, bulkDataObjRegInp,
                                   &bulkDataObjRegOut );
        if ( status < 0 ) {
            log_api::error("bulkRegSubfile: rsBulkDataObjReg error for {}. status = {}", subfilePath, status);
            cleanupBulkRegFiles( rsComm, bulkDataObjRegInp );
        }
        postProcRenamedPhyFiles( renamedPhyFiles, status );
        postProcBulkPut( rsComm, bulkDataObjRegInp, bulkDataObjRegOut );
        freeGenQueryOut( &bulkDataObjRegOut );
        bulkDataObjRegInp->rowCnt = 0;
    }
    return status;
}

int
addRenamedPhyFile( char *subObjPath, char *oldFileName, char *newFileName,
                   renamedPhyFiles_t *renamedPhyFiles ) {
    if ( subObjPath == NULL || oldFileName == NULL || newFileName == NULL ||
            renamedPhyFiles == NULL ) {
        return USER__NULL_INPUT_ERR;
    }

    if ( renamedPhyFiles->count >= MAX_NUM_BULK_OPR_FILES ) {
        log_api::error("addRenamedPhyFile: count >= {} for {}", MAX_NUM_BULK_OPR_FILES, subObjPath);
        return SYS_RENAME_STRUCT_COUNT_EXCEEDED;
    }
    rstrcpy( &renamedPhyFiles->objPath[renamedPhyFiles->count][0],
             subObjPath, MAX_NAME_LEN );
    rstrcpy( &renamedPhyFiles->origFilePath[renamedPhyFiles->count][0],
             oldFileName, MAX_NAME_LEN );
    rstrcpy( &renamedPhyFiles->newFilePath[renamedPhyFiles->count][0],
             newFileName, MAX_NAME_LEN );
    renamedPhyFiles->count++;
    return 0;
}

int
postProcRenamedPhyFiles( renamedPhyFiles_t *renamedPhyFiles, int regStatus ) {
    int i;
    int status = 0;
    int savedStatus = 0;

    if ( renamedPhyFiles == NULL ) {
        return USER__NULL_INPUT_ERR;
    }

    if ( regStatus >= 0 ) {
        for ( i = 0; i < renamedPhyFiles->count; i++ ) {
            unlink( &renamedPhyFiles->newFilePath[i][0] );
        }
    }
    else {
        /* restore the phy files */
        for ( i = 0; i < renamedPhyFiles->count; i++ ) {
            status = rename( &renamedPhyFiles->newFilePath[i][0],
                             &renamedPhyFiles->origFilePath[i][0] );
            if ( status < 0 ) {
                irods::log( status, "rename failed." );
            }
            savedStatus = UNIX_FILE_RENAME_ERR - errno;
            log_api::error(
                "postProcRenamedPhyFiles: rename error from {} to {}, status={}",
                &renamedPhyFiles->newFilePath[i][0],
                &renamedPhyFiles->origFilePath[i][0],
                savedStatus);
        }
    }

    std::memset(renamedPhyFiles, 0, sizeof(renamedPhyFiles_t));

    return savedStatus;
}

int
cleanupBulkRegFiles( rsComm_t *rsComm, genQueryOut_t *bulkDataObjRegInp ) {
    sqlResult_t *filePath, *rescName;
    char *tmpFilePath, *tmpRescName;
    int i;

    if ( bulkDataObjRegInp == NULL ) {
        return USER__NULL_INPUT_ERR;
    }

    if ( ( filePath =
                getSqlResultByInx( bulkDataObjRegInp, COL_D_DATA_PATH ) ) == NULL ) {
        log_api::info("cleanupBulkRegFiles: getSqlResultByInx for COL_D_DATA_PATH failed");
        return UNMATCHED_KEY_OR_INDEX;
    }
    if ( ( rescName =
                getSqlResultByInx( bulkDataObjRegInp, COL_D_RESC_NAME ) ) == NULL ) {
        log_api::info("rsBulkDataObjReg: getSqlResultByInx for COL_D_RESC_NAME failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    for ( i = 0; i < bulkDataObjRegInp->rowCnt; i++ ) {
        tmpFilePath = &filePath->value[filePath->len * i];
        tmpRescName = &rescName->value[rescName->len * i];
        /* make sure it is an orphan file */
        if ( chkOrphanFile( rsComm, tmpFilePath, tmpRescName, NULL ) > 0 ) {
            unlink( tmpFilePath );
        }
    }

    return 0;
}

int
postProcBulkPut( rsComm_t *rsComm, genQueryOut_t *bulkDataObjRegInp,
                 genQueryOut_t *bulkDataObjRegOut ) {
    dataObjInfo_t dataObjInfo;
    sqlResult_t *objPath, *dataType, *dataSize, *rescName, *filePath,
                *dataMode, *oprType, *replNum, *chksum;
    char *tmpObjPath, *tmpDataType, *tmpDataSize, *tmpFilePath,
         *tmpDataMode, *tmpReplNum, *tmpChksum;
    sqlResult_t *objId;
    int status, i;
    dataObjInp_t dataObjInp;
    ruleExecInfo_t rei;
    int savedStatus = 0;

    if ( bulkDataObjRegInp == NULL || bulkDataObjRegOut == NULL ) {
        return USER__NULL_INPUT_ERR;
    }

    initReiWithDataObjInp( &rei, rsComm, NULL );
    status = applyRule( "acBulkPutPostProcPolicy", NULL, &rei, NO_SAVE_REI );
    clearKeyVal(rei.condInputData);
    free(rei.condInputData);
    if ( status < 0 ) {
        log_api::error("postProcBulkPut: acBulkPutPostProcPolicy error status = {}", status);
        return status;
    }

    if ( rei.status == POLICY_OFF ) {
        return 0;
    }

    if ( ( objPath =
                getSqlResultByInx( bulkDataObjRegInp, COL_DATA_NAME ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_DATA_NAME failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    if ( ( dataType =
                getSqlResultByInx( bulkDataObjRegInp, COL_DATA_TYPE_NAME ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_DATA_TYPE_NAME failed");
        return UNMATCHED_KEY_OR_INDEX;
    }
    if ( ( dataSize =
                getSqlResultByInx( bulkDataObjRegInp, COL_DATA_SIZE ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_DATA_SIZE failed");
        return UNMATCHED_KEY_OR_INDEX;
    }
    if ( ( rescName =
                getSqlResultByInx( bulkDataObjRegInp, COL_D_RESC_NAME ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_D_RESC_NAME failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    if ( ( filePath =
                getSqlResultByInx( bulkDataObjRegInp, COL_D_DATA_PATH ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_D_DATA_PATH failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    if ( ( dataMode =
                getSqlResultByInx( bulkDataObjRegInp, COL_DATA_MODE ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_DATA_MODE failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    if ( ( oprType =
                getSqlResultByInx( bulkDataObjRegInp, OPR_TYPE_INX ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for OPR_TYPE_INX failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    if ( ( replNum =
                getSqlResultByInx( bulkDataObjRegInp, COL_DATA_REPL_NUM ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_DATA_REPL_NUM failed");
        return UNMATCHED_KEY_OR_INDEX;
    }
    chksum = getSqlResultByInx( bulkDataObjRegInp, COL_D_DATA_CHECKSUM );

    /* the output */
    if ( ( objId =
                getSqlResultByInx( bulkDataObjRegOut, COL_D_DATA_ID ) ) == NULL ) {
        log_api::error("postProcBulkPut: getSqlResultByInx for COL_D_DATA_ID failed");
        return UNMATCHED_KEY_OR_INDEX;
    }

    /* create a template */
    std::memset(&dataObjInfo, 0, sizeof(dataObjInfo_t));
    rstrcpy( dataObjInfo.rescName, rescName->value, NAME_LEN );
    dataObjInfo.replStatus = INTERMEDIATE_REPLICA;
    /*status = resolveResc (rescName->value, &dataObjInfo.rescInfo);
      if (status < 0) {
      log_api::error("postProcBulkPut: resolveResc error for {}, status = {}",      rescName->value, status);
      return status;
      }*/

    std::memset(&dataObjInp, 0, sizeof(dataObjInp_t));
    dataObjInp.openFlags = O_WRONLY;

    for ( i = 0; i < bulkDataObjRegInp->rowCnt; i++ ) {
        dataObjInfo_t *tmpDataObjInfo;

        tmpDataObjInfo = ( dataObjInfo_t * )malloc( sizeof( dataObjInfo_t ) );
        if ( tmpDataObjInfo == NULL ) {
            return SYS_MALLOC_ERR;
        }

        *tmpDataObjInfo = dataObjInfo;

        tmpObjPath = &objPath->value[objPath->len * i];
        tmpDataType = &dataType->value[dataType->len * i];
        tmpDataSize = &dataSize->value[dataSize->len * i];
        tmpFilePath = &filePath->value[filePath->len * i];
        tmpDataMode = &dataMode->value[dataMode->len * i];
        tmpReplNum =  &replNum->value[replNum->len * i];

        rstrcpy( tmpDataObjInfo->objPath, tmpObjPath, MAX_NAME_LEN );
        rstrcpy( dataObjInp.objPath, tmpObjPath, MAX_NAME_LEN );
        rstrcpy( tmpDataObjInfo->dataType, tmpDataType, NAME_LEN );
        tmpDataObjInfo->dataSize = strtoll( tmpDataSize, 0, 0 );
        rstrcpy( tmpDataObjInfo->filePath, tmpFilePath, MAX_NAME_LEN );
        rstrcpy( tmpDataObjInfo->dataMode, tmpDataMode, NAME_LEN );
        tmpDataObjInfo->replNum = atoi( tmpReplNum );
        if ( chksum != NULL ) {
            tmpChksum = &chksum->value[chksum->len * i];
            if ( strlen( tmpChksum ) > 0 ) {
                rstrcpy( tmpDataObjInfo->chksum, tmpChksum, NAME_LEN );
            }
        }
        initReiWithDataObjInp( &rei, rsComm, &dataObjInp );
        rei.doi = tmpDataObjInfo;

        // make resource properties available as rule session variables
        irods::get_resc_properties_as_kvp(rei.doi->rescHier, rei.condInputData);

        status = applyRule( "acPostProcForPut", NULL, &rei, NO_SAVE_REI );
        clearKeyVal(rei.condInputData);
        free(rei.condInputData);
        if ( status < 0 ) {
            savedStatus = status;
        }

        freeAllDataObjInfo( rei.doi );
    }
    return savedStatus;
}
