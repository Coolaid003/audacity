/**********************************************************************

Audacity: A Digital Audio Editor

ProjectFileIO.cpp

Paul Licameli split from AudacityProject.cpp

**********************************************************************/

#include "ProjectFileIO.h"


#include <sqlite3.h>
#include <wx/crt.h>
#include <wx/frame.h>
#include <wx/progdlg.h>
#include <wx/sstream.h>
#include <wx/xml/xml.h>

#include "FileNames.h"
#include "Project.h"
#include "ProjectFileIORegistry.h"
#include "ProjectSerializer.h"
#include "ProjectSettings.h"
#include "SampleBlock.h"
#include "Sequence.h"
#include "Tags.h"
#include "TimeTrack.h"
#include "ViewInfo.h"
#include "WaveClip.h"
#include "WaveTrack.h"
#include "widgets/AudacityMessageBox.h"
#include "widgets/NumericTextCtrl.h"
#include "widgets/ProgressDialog.h"
#include "xml/XMLFileReader.h"

wxDEFINE_EVENT(EVT_PROJECT_TITLE_CHANGE, wxCommandEvent);

static const int ProjectFileID = ('A' << 24 | 'U' << 16 | 'D' << 8 | 'Y');
static const int ProjectFileVersion = 1;

// Navigation:
//
// Bindings are marked out in the code by, e.g. 
// BIND SQL sampleblocks
// A search for "BIND SQL" will find all bindings.
// A search for "SQL sampleblocks" will find all SQL related 
// to sampleblocks.

static const char *ProjectFileSchema =
   // These are persistent and not connection based
   //
   // See the CMakeList.txt for the SQLite lib for more
   // settings.
   "PRAGMA <schema>.application_id = %d;"
   "PRAGMA <schema>.user_version = %d;"
   ""
   // project is a binary representation of an XML file.
   // it's in binary for speed.
   // One instance only.  id is always 1.
   // dict is a dictionary of fieldnames.
   // doc is the binary representation of the XML
   // in the doc, fieldnames are replaced by 2 byte dictionary
   // index numbers.
   // This is all opaque to SQLite.  It just sees two
   // big binary blobs.
   // There is no limit to document blob size.
   // dict will be smallish, with an entry for each 
   // kind of field.
   "CREATE TABLE IF NOT EXISTS <schema>.project"
   "("
   "  id                   INTEGER PRIMARY KEY,"
   "  dict                 BLOB,"
   "  doc                  BLOB"
   ");"
   ""
   // CREATE SQL autosave
   // autosave is a binary representation of an XML file.
   // it's in binary for speed.
   // One instance only.  id is always 1.
   // dict is a dictionary of fieldnames.
   // doc is the binary representation of the XML
   // in the doc, fieldnames are replaced by 2 byte dictionary
   // index numbers.
   // This is all opaque to SQLite.  It just sees two
   // big binary blobs.
   // There is no limit to document blob size.
   // dict will be smallish, with an entry for each 
   // kind of field.
   "CREATE TABLE IF NOT EXISTS <schema>.autosave"
   "("
   "  id                   INTEGER PRIMARY KEY,"
   "  dict                 BLOB,"
   "  doc                  BLOB"
   ");"
   ""
   // CREATE SQL tags
   // tags is not used (yet)
   "CREATE TABLE IF NOT EXISTS <schema>.tags"
   "("
   "  name                 TEXT,"
   "  value                BLOB"
   ");"
   ""
   // CREATE SQL sampleblocks
   // 'samples' are fixed size blocks of int16, int32 or float32 numbers.
   // The blocks may be partially empty.
   // The quantity of valid data in the blocks is
   // provided in the project blob.
   // 
   // sampleformat specifies the format of the samples stored.
   //
   // blockID is a 64 bit number.
   //
   // Rows are immutable -- never updated after addition, but may be
   // deleted.
   //
   // summin to summary64K are summaries at 3 distance scales.
   "CREATE TABLE IF NOT EXISTS <schema>.sampleblocks"
   "("
   "  blockid              INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  sampleformat         INTEGER,"
   "  summin               REAL,"
   "  summax               REAL,"
   "  sumrms               REAL,"
   "  summary256           BLOB,"
   "  summary64k           BLOB,"
   "  samples              BLOB"
   ");";

// Configuration to provide "safe" connections
static const char *SafeConfig =
   "PRAGMA <schema>.locking_mode = SHARED;"
   "PRAGMA <schema>.synchronous = NORMAL;"
   "PRAGMA <schema>.journal_mode = WAL;"
   "PRAGMA <schema>.wal_autocheckpoint = 0;";

// Configuration to provide "Fast" connections
static const char *FastConfig =
   "PRAGMA <schema>.locking_mode = SHARED;"
   "PRAGMA <schema>.synchronous = OFF;"
   "PRAGMA <schema>.journal_mode = OFF;";

// This singleton handles initialization/shutdown of the SQLite library.
// It is needed because our local SQLite is built with SQLITE_OMIT_AUTOINIT
// defined.
//
// It's safe to use even if a system version of SQLite is used that didn't
// have SQLITE_OMIT_AUTOINIT defined.
class SQLiteIniter
{
public:
   SQLiteIniter()
   {
      // Enable URI filenames for all connections
      mRc = sqlite3_config(SQLITE_CONFIG_URI, 1);

      if (mRc == SQLITE_OK)
      {
         mRc = sqlite3_initialize();
      }

#if !defined(__WXMSW__)
      if (mRc == SQLITE_OK)
      {
         // Use the "unix-excl" VFS to make access to the DB exclusive.  This gets
         // rid of the "<database name>-shm" shared memory file.
         //
         // Though it shouldn't, it doesn't matter if this fails.
         auto vfs = sqlite3_vfs_find("unix-excl");
         if (vfs)
         {
            sqlite3_vfs_register(vfs, 1);
         }
      }
#endif
   }
   ~SQLiteIniter()
   {
      // This function must be called single-threaded only
      // It returns a value, but there's nothing we can do with it
      (void) sqlite3_shutdown();
   }
   int mRc;
};

bool ProjectFileIO::InitializeSQL()
{
   static SQLiteIniter sqliteIniter;
   return sqliteIniter.mRc == SQLITE_OK;
}

static void RefreshAllTitles(bool bShowProjectNumbers )
{
   for ( auto pProject : AllProjects{} ) {
      if ( !GetProjectFrame( *pProject ).IsIconized() ) {
         ProjectFileIO::Get( *pProject ).SetProjectTitle(
            bShowProjectNumbers ? pProject->GetProjectNumber() : -1 );
      }
   }
}

TitleRestorer::TitleRestorer(
   wxTopLevelWindow &window, AudacityProject &project )
{
   if( window.IsIconized() )
      window.Restore();
   window.Raise(); // May help identifying the window on Mac

   // Construct this project's name and number.
   sProjName = project.GetProjectName();
   if ( sProjName.empty() ) {
      sProjName = _("<untitled>");
      UnnamedCount = std::count_if(
         AllProjects{}.begin(), AllProjects{}.end(),
         []( const AllProjects::value_type &ptr ){
            return ptr->GetProjectName().empty();
         }
      );
      if ( UnnamedCount > 1 ) {
         sProjNumber.Printf(
            _("[Project %02i] "), project.GetProjectNumber() + 1 );
         RefreshAllTitles( true );
      } 
   }
   else
      UnnamedCount = 0;
}

TitleRestorer::~TitleRestorer() {
   if( UnnamedCount > 1 )
      RefreshAllTitles( false );
}

static const AudacityProject::AttachedObjects::RegisteredFactory sFileIOKey{
   []( AudacityProject &parent ){
      auto result = std::make_shared< ProjectFileIO >( parent );
      return result;
   }
};

ProjectFileIO &ProjectFileIO::Get( AudacityProject &project )
{
   auto &result = project.AttachedObjects::Get< ProjectFileIO >( sFileIOKey );
   return result;
}

const ProjectFileIO &ProjectFileIO::Get( const AudacityProject &project )
{
   return Get( const_cast< AudacityProject & >( project ) );
}

ProjectFileIO::ProjectFileIO(AudacityProject &)
{
   mPrevDB = nullptr;
   mDB = nullptr;

   mRecovered = false;
   mModified = false;
   mTemporary = true;
   mBypass = false;

   UpdatePrefs();
}

void ProjectFileIO::Init( AudacityProject &project )
{
   // This step can't happen in the ctor of ProjectFileIO because ctor of
   // AudacityProject wasn't complete
   mpProject = project.shared_from_this();

   // Kick off the checkpoint thread
   mCheckpointThread = std::thread([this]{ CheckpointThread(); });
}

ProjectFileIO::~ProjectFileIO()
{
   wxASSERT_MSG(mDB == nullptr, wxT("Project file was not closed at shutdown"));

   // Tell the checkpoint thread to shutdown
   {
      std::lock_guard<std::mutex> guard(mCheckpointMutex);
      mCheckpointStop = true;
      mCheckpointCondition.notify_one();
   }

   // And wait for it to do so
   mCheckpointThread.join();
}

void ProjectFileIO::CheckpointThread()
{
   mCheckpointStop = false;

   while (true)
   {
      {
         // Wait for work or the stop signal
         std::unique_lock<std::mutex> lock(mCheckpointMutex);
         mCheckpointCondition.wait(lock,
                                   [&]
                                   {
                                       return mCheckpointWaitingPages || mCheckpointStop;
                                   });

         // Requested to stop, so bail
         if (mCheckpointStop)
         {
            break;
         }

         // Capture the number of pages that need checkpointing and reset
         mCheckpointCurrentPages.store( mCheckpointWaitingPages );
         mCheckpointWaitingPages = 0;
      }

      // Open another connection to the DB to prevent blocking the main thread.
      sqlite3 *db = nullptr;
      if (sqlite3_open(mFileName, &db) == SQLITE_OK)
      {
         // Configure it to be safe
         Config(db, SafeConfig);

         // And kick off the checkpoint. This may not checkpoint ALL frames
         // in the WAL.  They'll be gotten the next time around.
         sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);

         // All done.
         sqlite3_close(db);

         // Reset
         mCheckpointCurrentPages = 0;
      }
      else
         // Gotta close it anyway!
         sqlite3_close( db );
   }

   return;
}

int ProjectFileIO::CheckpointHook(void *data, sqlite3 *db, const char *schema, int pages)
{
   // Get access to our object
   ProjectFileIO *that = static_cast<ProjectFileIO *>(data);

   // Queue the database pointer for our checkpoint thread to process
   std::lock_guard<std::mutex> guard(that->mCheckpointMutex);
   that->mCheckpointWaitingPages = pages;
   that->mCheckpointCondition.notify_one();

   return SQLITE_OK;
}

sqlite3 *ProjectFileIO::DB()
{
   if (!mDB)
   {
      if (!OpenDB())
      {
         throw SimpleMessageBoxException
         {
            XO("Failed to open the project's database")
         };
      }
   }

   return mDB;
}

// Put the current database connection aside, keeping it open, so that
// another may be opened with OpenDB()
void ProjectFileIO::SaveConnection()
{
   // Should do nothing in proper usage, but be sure not to leak a connection:
   DiscardConnection();

   mPrevDB = mDB;
   mPrevFileName = mFileName;

   mDB = nullptr;
   SetFileName({});
}

// Close any set-aside connection
void ProjectFileIO::DiscardConnection()
{
   if ( mPrevDB )
   {
      auto rc = sqlite3_close( mPrevDB );
      if ( rc != SQLITE_OK )
      {
         // Store an error message
         SetDBError(
            XO("Failed to successfully close the source project file")
         );
      }
      mPrevDB = nullptr;
      mPrevFileName.clear();
   }
}

// Close any current connection and switch back to using the saved
void ProjectFileIO::RestoreConnection()
{
   if ( mDB )
   {
      auto rc = sqlite3_close( mDB );
      if ( rc != SQLITE_OK )
      {
         // Store an error message
         SetDBError(
            XO("Failed to successfully close the destination project file")
         );
      }
   }
   mDB = mPrevDB;
   SetFileName(mPrevFileName);

   mPrevDB = nullptr;
   mPrevFileName.clear();
}

void ProjectFileIO::UseConnection( sqlite3 *db, const FilePath &filePath )
{
   wxASSERT(mDB == nullptr);
   mDB = db;
   SetFileName( filePath );
}

void ProjectFileIO::Config(sqlite3 *db, const char *config, const wxString &schema)
{
   int rc;

   wxString sql = config;

   if (schema.empty())
   {
      sql.Replace(wxT("<schema>."), wxT(""));
   }
   else
   {
      sql.Replace(wxT("<schema>"), schema);
   }

   rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      // This non-fatal...for now
      SetDBError(XO("Failed to set connection configuration"));
   }

   return;
}

sqlite3 *ProjectFileIO::OpenDB(FilePath fileName)
{
   wxASSERT(mDB == nullptr);
   bool temp = false;

   if (fileName.empty())
   {
      fileName = GetFileName();
      if (fileName.empty())
      {
         fileName = FileNames::UnsavedProjectFileName();
         temp = true;
      }
      else
      {
         temp = false;
      }
   }

   int rc = sqlite3_open(fileName, &mDB);
   if (rc != SQLITE_OK)
   {
      SetDBError(XO("Failed to open project file"));
      // sqlite3 docs say you should close anyway to avoid leaks
      sqlite3_close( mDB );
      mDB = nullptr;
      return nullptr;
   }

   // Ensure attached DB connection gets configured
   Config(mDB, SafeConfig);

   if (!CheckVersion())
   {
      CloseDB();
      return nullptr;
   }

   mTemporary = temp;

   SetFileName(fileName);

   // Install our checkpoint hook
   sqlite3_wal_hook(mDB, CheckpointHook, this);

   return mDB;
}

bool ProjectFileIO::CloseDB()
{
   int rc;

   if (mDB)
   {
      // Uninstall our checkpoint hook so that no additional checkpoints
      // are sent our way.  (Though this shouldn't really happen.)
      sqlite3_wal_hook(mDB, nullptr, nullptr);

      // Display a progress dialog if there's active or pending checkpoints
      if (mCheckpointWaitingPages || mCheckpointCurrentPages)
      {
         TranslatableString title = XO("Checkpointing project");

         // Get access to the active tracklist
         auto pProject = mpProject.lock();
         if (pProject)
         {
            title = XO("Checkpointing %s").Format(pProject->GetProjectName());
         }

         wxGenericProgressDialog pd(title.Translation(),
                                    XO("This may take several seconds").Translation(),
                                    300000,     // range
                                    nullptr,    // parent
                                    wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_SMOOTH);

         while (mCheckpointWaitingPages || mCheckpointCurrentPages)
         {
            wxMilliSleep(50);
            pd.Pulse();
         }
      }

      // Close the DB
      rc = sqlite3_close(mDB);
      if (rc != SQLITE_OK)
      {
         SetDBError(XO("Failed to close the project file"));
      }

      mDB = nullptr;
      SetFileName({});
   }

   return true;
}

bool ProjectFileIO::DeleteDB()
{
   wxASSERT(mDB == nullptr);

   if (mTemporary && !mFileName.empty())
   {
      wxFileName temp(FileNames::TempDir());
      if (temp == wxPathOnly(mFileName))
      {
         if (!wxRemoveFile(mFileName))
         {
            SetError(XO("Failed to close the project file"));

            return false;
         }
      }
   }

   return true;
}

bool ProjectFileIO::TransactionStart(const wxString &name)
{
   char* errmsg = nullptr;

   int rc = sqlite3_exec(DB(),
                         wxT("SAVEPOINT ") + name + wxT(";"),
                         nullptr,
                         nullptr,
                         &errmsg);

   if (errmsg)
   {
      SetDBError(
         XO("Failed to create savepoint:\n\n%s").Format(name)
      );
      sqlite3_free(errmsg);
   }

   return rc == SQLITE_OK;
}

bool ProjectFileIO::TransactionCommit(const wxString &name)
{
   char* errmsg = nullptr;

   int rc = sqlite3_exec(DB(),
                         wxT("RELEASE ") + name + wxT(";"),
                         nullptr,
                         nullptr,
                         &errmsg);

   if (errmsg)
   {
      SetDBError(
         XO("Failed to release savepoint:\n\n%s").Format(name)
      );
      sqlite3_free(errmsg);
   }

   return rc == SQLITE_OK;
}

bool ProjectFileIO::TransactionRollback(const wxString &name)
{
   char* errmsg = nullptr;

   int rc = sqlite3_exec(DB(),
                         wxT("ROLLBACK TO ") + name + wxT(";"),
                         nullptr,
                         nullptr,
                         &errmsg);

   if (errmsg)
   {
      SetDBError(
         XO("Failed to release savepoint:\n\n%s").Format(name)
      );
      sqlite3_free(errmsg);
   }

   return rc == SQLITE_OK;
}

static int ExecCallback(void *data, int cols, char **vals, char **names)
{
   auto &cb = *static_cast<const ProjectFileIO::ExecCB *>(data);
   // Be careful not to throw anything across sqlite3's stack frames.
   return GuardedCall<int>(
      [&]{ return cb(cols, vals, names); },
      MakeSimpleGuard( 1 )
   );
}

int ProjectFileIO::Exec(const char *query, const ExecCB &callback)
{
   char *errmsg = nullptr;

   const void *ptr = &callback;
   int rc = sqlite3_exec(DB(), query, ExecCallback,
                         const_cast<void*>(ptr), &errmsg);

   if (rc != SQLITE_ABORT && errmsg)
   {
      SetDBError(
         XO("Failed to execute a project file command:\n\n%s").Format(query)
      );
      mLibraryError = Verbatim(errmsg);
   }
   if (errmsg)
   {
      sqlite3_free(errmsg);
   }

   return rc;
}

bool ProjectFileIO::Query(const char *sql, const ExecCB &callback)
{
   int rc = Exec(sql, callback);
   // SQLITE_ABORT is a non-error return only meaning the callback
   // stopped the iteration of rows early
   if ( !(rc == SQLITE_OK || rc == SQLITE_ABORT) )
   {
      return false;
   }

   return true;
}

bool ProjectFileIO::GetValue(const char *sql, wxString &result)
{
   // Retrieve the first column in the first row, if any
   result.clear();
   auto cb = [&result](int cols, char **vals, char **){
      if (cols > 0)
         result = vals[0];
      // Stop after one row
      return 1;
   };

   return Query(sql, cb);
}

bool ProjectFileIO::GetBlob(const char *sql, wxMemoryBuffer &buffer)
{
   auto db = DB();
   int rc;

   buffer.Clear();

   sqlite3_stmt *stmt = nullptr;
   auto cleanup = finally([&]
   {
      if (stmt)
      {
         sqlite3_finalize(stmt);
      }
   });

   rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Unable to prepare project file command:\n\n%s").Format(sql)
      );
      return false;
   }

   rc = sqlite3_step(stmt);

   // A row wasn't found...not an error
   if (rc == SQLITE_DONE)
   {
      return true;
   }

   if (rc != SQLITE_ROW)
   {
      SetDBError(
         XO("Failed to retrieve data from the project file.\nThe following command failed:\n\n%s").Format(sql)
      );
      // AUD TODO handle error
      return false;
   }

   const void *blob = sqlite3_column_blob(stmt, 0);
   int size = sqlite3_column_bytes(stmt, 0);

   buffer.AppendData(blob, size);

   return true;
}

bool ProjectFileIO::CheckVersion()
{
   auto db = DB();
   int rc;

   // Install our schema if this is an empty DB
   wxString result;
   if (!GetValue("SELECT Count(*) FROM sqlite_master WHERE type='table';", result))
   {
      return false;
   }

   // If the return count is zero, then there are no tables defined, so this
   // must be a new project file.
   if (wxStrtol<char **>(result, nullptr, 10) == 0)
   {
      return InstallSchema(db);
   }

   // Check for our application ID
   if (!GetValue("PRAGMA application_ID;", result))
   {
      return false;
   }

   // It's a database that SQLite recognizes, but it's not one of ours
   if (wxStrtoul<char **>(result, nullptr, 10) != ProjectFileID)
   {
      SetError(XO("This is not an Audacity project file"));
      return false;
   }

   // Get the project file version
   if (!GetValue("PRAGMA user_version;", result))
   {
      return false;
   }

   long version = wxStrtol<char **>(result, nullptr, 10);

   // Project file version is higher than ours. We will refuse to
   // process it since we can't trust anything about it.
   if (version > ProjectFileVersion)
   {
      SetError(
         XO("This project was created with a newer version of Audacity:\n\nYou will need to upgrade to process it")
      );
      return false;
   }
   
   // Project file is older than ours, ask the user if it's okay to
   // upgrade.
   if (version < ProjectFileVersion)
   {
      return UpgradeSchema();
   }

   return true;
}

bool ProjectFileIO::InstallSchema(sqlite3 *db, const char *schema /* = "main" */)
{
   int rc;

   wxString sql;
   sql.Printf(ProjectFileSchema, ProjectFileID, ProjectFileVersion);
   sql.Replace("<schema>", schema);

   rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Unable to initialize the project file")
      );
      return false;
   }

   return true;
}

bool ProjectFileIO::UpgradeSchema()
{
   // To do
   return true;
}

// The orphan block handling should be removed once autosave and related
// blocks become part of the same transaction.

// An SQLite function that takes a blockid and looks it up in a set of
// blockids captured during project load.  If the blockid isn't found
// in the set, it will be deleted.
void ProjectFileIO::InSet(sqlite3_context *context, int argc, sqlite3_value **argv)
{
   BlockIDs *blockids = (BlockIDs *) sqlite3_user_data(context);
   SampleBlockID blockid = sqlite3_value_int64(argv[0]);

   sqlite3_result_int(context, blockids->find(blockid) != blockids->end());
}

bool ProjectFileIO::CheckForOrphans(BlockIDs &blockids)
{
   auto db = DB();
   int rc;

   auto cleanup = finally([&]
   {
      // Remove our function, whether it was successfully defined or not.
      sqlite3_create_function(db, "inset", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, nullptr, nullptr, nullptr);
   });

   // Add the function used to verify each row's blockid against the set of active blockids
   rc = sqlite3_create_function(db, "inset", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, &blockids, InSet, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      wxLogDebug(wxT("Unable to add 'inset' function"));
      return false;
   }

   // Delete all rows that are orphaned
   rc = sqlite3_exec(db, "DELETE FROM sampleblocks WHERE NOT inset(blockid);", nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      wxLogWarning(XO("Cleanup of orphan blocks failed").Translation());
      return false;
   }

   // Mark the project recovered if we deleted any rows
   int changes = sqlite3_changes(db);
   if (changes > 0)
   {
      wxLogInfo(XO("Total orphan blocks deleted %d").Translation(), changes);
      mRecovered = true;
   }

   return true;
}

sqlite3 *ProjectFileIO::CopyTo(const FilePath &destpath,
                               const TranslatableString &msg,
                               bool prune /* = false */,
                               const std::shared_ptr<TrackList> &tracks/* = nullptr */)
{
   // Get access to the active tracklist
   auto pProject = mpProject.lock();
   if (!pProject)
   {
      return nullptr;
   }
   auto &tracklist = tracks ? *tracks : TrackList::Get(*pProject);

   BlockIDs blockids;

   // Collect all active blockids
   if (prune)
   {
      for (auto wt : tracklist.Any<const WaveTrack>())
      {
         // Scan all clips within current track
         for (const auto &clip : wt->GetAllClips())
         {
            // Scan all sample blocks within current clip
            auto blocks = clip->GetSequenceBlockArray();
            for (const auto &block : *blocks)
            {
               blockids.insert(block.sb->GetBlockID());
            }
         }
      }
   }
   // Collect ALL blockids
   else
   {
      auto cb = [&blockids](int cols, char **vals, char **){
         SampleBlockID blockid;
         wxString{ vals[0] }.ToLongLong(&blockid);
         blockids.insert(blockid);
         return 0;
      };

      if (!Query("SELECT blockid FROM sampleblocks;", cb))
      {
         return nullptr;
      }
   }

   // Create the project doc
   ProjectSerializer doc;
   WriteXMLHeader(doc);
   WriteXML(doc, false, tracks);

   auto db = DB();
   sqlite3 *destdb = nullptr;
   bool success = false;
   int rc;
   ProgressResult res = ProgressResult::Success;

   // Cleanup in case things go awry
   auto cleanup = finally([&]
   {
      if (!success)
      {
         sqlite3_close(destdb);

         sqlite3_exec(db, "DETACH DATABASE outbound;", nullptr, nullptr, nullptr);

         wxRemoveFile(destpath);
      }
   });

   // Attach the destination database 
   wxString sql;
   sql.Printf("ATTACH DATABASE '%s' AS outbound;", destpath);

   rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Unable to attach destination database")
      );
      return nullptr;
   }

   // Ensure attached DB connection gets configured
   Config(db, FastConfig, "outbound");

   // Install our schema into the new database
   if (!InstallSchema(db, "outbound"))
   {
      // Message already set
      return nullptr;
   }

   // Copy over tags (not really used yet)
   rc = sqlite3_exec(db,
                     "INSERT INTO outbound.tags SELECT * FROM main.tags;",
                     nullptr,
                     nullptr,
                     nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Failed to copy tags")
      );

      return nullptr;
   }

   {
      // Ensure statement gets cleaned up
      sqlite3_stmt *stmt = nullptr;
      auto cleanup = finally([&]
      {
         if (stmt)
         {
            sqlite3_finalize(stmt);
         }
      });

      // Prepare the statement only once
      rc = sqlite3_prepare_v2(db,
                              "INSERT INTO outbound.sampleblocks"
                              "  SELECT * FROM main.sampleblocks"
                              "  WHERE blockid = ?;",
                              -1,
                              &stmt,
                              nullptr);
      if (rc != SQLITE_OK)
      {
         SetDBError(
            XO("Unable to prepare project file command:\n\n%s").Format(sql)
         );
         return nullptr;
      }

      /* i18n-hint: This title appears on a dialog that indicates the progress
         in doing something.*/
      ProgressDialog progress(XO("Progress"), msg, pdlgHideStopButton);
      ProgressResult result = ProgressResult::Success;

      wxLongLong_t count = 0;
      wxLongLong_t total = blockids.size();

      // Start a transaction.  Since we're running without a journal,
      // this really doesn't provide rollback.  It just prevents SQLite
      // from auto committing after each step through the loop.
      sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);

      // Copy sample blocks from the main DB to the outbound DB
      for (auto blockid : blockids)
      {
         // BIND blockid parameter
         if (sqlite3_bind_int64(stmt, 1, blockid) != SQLITE_OK)
         {
            THROW_INCONSISTENCY_EXCEPTION;
         }

         // Process it
         rc = sqlite3_step(stmt);
         if (rc != SQLITE_DONE)
         {
            SetDBError(
               XO("Failed to update the project file.\nThe following command failed:\n\n%s").Format(sql)
            );
            return nullptr;
         }

         // BIND blockid parameter
         if (sqlite3_reset(stmt) != SQLITE_OK)
         {
            THROW_INCONSISTENCY_EXCEPTION;
         }

         result = progress.Update(++count, total);
         if (result != ProgressResult::Success)
         {
            // Note that we're not setting success, so the finally
            // block above will take care of cleaning up
            return nullptr;
         }
      }

      // See BEGIN above...
      sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
   }

   // Detach the destination database
   rc = sqlite3_exec(db, "DETACH DATABASE outbound;", nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Destination project could not be detached")
      );

      return nullptr;
   }

   // Open the newly created database
   rc = sqlite3_open(destpath, &destdb);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Failed to open copy of project file")
      );

      return nullptr;
   }

   // Ensure attached DB connection gets configured
   Config(destdb, SafeConfig);

   // Write the project doc
   if (!WriteDoc("project", doc, destdb))
   {
      return nullptr;
   }

   // Tell cleanup everything is good to go
   success = true;

   return destdb;
}

bool ProjectFileIO::ShouldVacuum(const std::shared_ptr<TrackList> &tracks)
{
   std::set<long long> active;
   unsigned long long current = 0;

   // Scan all wave tracks
   for (auto wt : tracks->Any<const WaveTrack>())
   {
      // Scan all clips within current track
      for (const auto &clip : wt->GetAllClips())
      {
         // Scan all sample blocks within current clip
         auto blocks = clip->GetSequenceBlockArray();
         for (const auto &block : *blocks)
         {
            const auto &sb = block.sb;
            auto blockid = sb->GetBlockID();

            // Accumulate space used by the block if the blockid has not
            // yet been seen
            if (active.count(blockid) == 0)
            {
               current += sb->GetSpaceUsage();

               active.insert(blockid);
            }
         }
      }
   }

   // Get the number of blocks and total length from the project file.
   unsigned long long blockcount = 0;
   unsigned long long total = 0;

   auto cb =
   [&blockcount, &total](int cols, char **vals, char **){
      if ( cols != 2 )
         // Should have two exactly!
         return 1;
      if ( total > 0 ) {
         // Should not have multiple rows!
         total = 0;
         return 1;
      }
      // Convert
      wxString{ vals[0] }.ToULongLong( &blockcount );
      wxString{ vals[1] }.ToULongLong( &total );
      return 0;
   };
   if (!Query("SELECT Count(*), "
     "Sum(Length(summary256)) + Sum(Length(summary64k)) + Sum(Length(samples)) "
     "FROM sampleblocks;", cb)
       || total == 0)
   {
      // Shouldn't vacuum since we don't have the full picture
      return false;
   }

   // Remember if we had unused blocks in the project file
   mHadUnused = (blockcount > active.size());

   // Let's make a percentage...should be plenty of head room
   current *= 100;

   wxLogDebug(wxT("used = %lld total = %lld %lld"), current, total, total ? current / total : 0);
   if (!total || current / total > 80)
   {
      wxLogDebug(wxT("not vacuuming"));
      return false;
   }
   wxLogDebug(wxT("vacuuming"));

   return true;
}

void ProjectFileIO::Vacuum(const std::shared_ptr<TrackList> &tracks)
{
   // Haven't vacuumed yet
   mWasVacuumed = false;

   // Assume we do until we found out otherwise. That way cleanup at project
   // close time will still occur
   mHadUnused = true;

   // Don't vacuum if this is a temporary project or if it's determined there are not
   // enough unused blocks to make it worthwhile
   if (IsTemporary() || !ShouldVacuum(tracks))
   {
      // Delete the AutoSave doc it if exists
      if (IsModified())
      {
         // PRL:  not clear what to do if the following fails, but the worst should
         // be, the project may reopen in its present state as a recovery file, not
         // at the last saved state.
         (void) AutoSaveDelete();
      }

      return;
   }

   // Create the project doc
   ProjectSerializer doc;
   WriteXMLHeader(doc);
   WriteXML(doc, false, tracks);

   wxString origName = mFileName;
   wxString tempName = origName + "_vacuum";

   // Must close the database to rename it
   if (!CloseDB())
   {
      return;
   }

   // Shouldn't need to do this, but doesn't hurt.
   wxRemoveFile(tempName);

   // If we can't rename the original to temporary, backout
   if (!wxRenameFile(origName, tempName))
   {
      OpenDB(origName);

      return;
   }

   // If we can't reopen the original database using the temporary name, backout
   sqlite3 *tempDB = nullptr;
   if (sqlite3_open(tempName, &tempDB) != SQLITE_OK)
   {
      SetDBError(XO("Failed to open project file"));
      // sqlite3 docs say you should close anyway to avoid leaks
      sqlite3_close( tempDB );

      wxRenameFile(tempName, origName);

      OpenDB(origName);

      return;
   }
   UseConnection(tempDB, tempName);

   // Ensure connection gets configured
   Config(mDB, SafeConfig);

   // Copy the original database to a new database while pruning unused sample blocks
   auto newDB = CopyTo(origName, XO("Compacting project"), true, tracks);

   // Close handle to the original database, even if the copy failed
   CloseDB();

   // Reestablish the original name.
   UseConnection(newDB, origName);

   // If the copy failed or we aren't able to write the project doc, backout
   if (!newDB)
   {
      // Close the new database
      sqlite3_close(newDB);

      // AUD3 warn user somehow
      wxRemoveFile(origName);

      // AUD3 warn user somehow
      wxRenameFile(tempName, origName);

      // Reopen original file
      OpenDB(origName);

      return;
   }

   wxRemoveFile(tempName);

   // Remember that we vacuumed
   mWasVacuumed = true;

   return;
}

bool ProjectFileIO::WasVacuumed()
{
   return mWasVacuumed;
}

bool ProjectFileIO::HadUnused()
{
   return mHadUnused;
}

void ProjectFileIO::UpdatePrefs()
{
   SetProjectTitle();
}

// Pass a number in to show project number, or -1 not to.
void ProjectFileIO::SetProjectTitle(int number)
{
   auto pProject = mpProject.lock();
   if (! pProject )
      return;

   auto &project = *pProject;
   auto pWindow = project.GetFrame();
   if (!pWindow)
   {
      return;
   }
   auto &window = *pWindow;
   wxString name = project.GetProjectName();

   // If we are showing project numbers, then we also explicitly show "<untitled>" if there
   // is none.
   if (number >= 0)
   {
      name =
      /* i18n-hint: The %02i is the project number, the %s is the project name.*/
      XO("[Project %02i] Audacity \"%s\"")
         .Format( number + 1,
                 name.empty() ? XO("<untitled>") : Verbatim((const char *)name))
         .Translation();
   }
   // If we are not showing numbers, then <untitled> shows as 'Audacity'.
   else if (name.empty())
   {
      name = _TS("Audacity");
   }

   if (mRecovered)
   {
      name += wxT(" ");
      /* i18n-hint: E.g this is recovered audio that had been lost.*/
      name += _("(Recovered)");
   }

   if (name != window.GetTitle())
   {
      window.SetTitle( name );
      window.SetName(name);       // to make the nvda screen reader read the correct title

      project.QueueEvent(
         safenew wxCommandEvent{ EVT_PROJECT_TITLE_CHANGE } );
   }
}

const FilePath &ProjectFileIO::GetFileName() const
{
   return mFileName;
}

void ProjectFileIO::SetFileName(const FilePath &fileName)
{
   auto pProject = mpProject.lock();
   if (! pProject )
      return;
   auto &project = *pProject;

   mFileName = fileName;

   if (mTemporary)
   {
      project.SetProjectName({});
   }
   else
   {
      project.SetProjectName(wxFileName(mFileName).GetName());
   }

   SetProjectTitle();
}

bool ProjectFileIO::HandleXMLTag(const wxChar *tag, const wxChar **attrs)
{
   auto pProject = mpProject.lock();
   if (! pProject )
      return false;
   auto &project = *pProject;
   auto &window = GetProjectFrame(project);
   auto &viewInfo = ViewInfo::Get(project);
   auto &settings = ProjectSettings::Get(project);

   wxString fileVersion;
   wxString audacityVersion;
   int requiredTags = 0;
   long longVpos = 0;

   // loop through attrs, which is a null-terminated list of
   // attribute-value pairs
   while (*attrs)
   {
      const wxChar *attr = *attrs++;
      const wxChar *value = *attrs++;

      if (!value || !XMLValueChecker::IsGoodString(value))
      {
         break;
      }

      if (viewInfo.ReadXMLAttribute(attr, value))
      {
         // We need to save vpos now and restore it below
         longVpos = std::max(longVpos, long(viewInfo.vpos));
         continue;
      }

      else if (!wxStrcmp(attr, wxT("version")))
      {
         fileVersion = value;
         requiredTags++;
      }

      else if (!wxStrcmp(attr, wxT("audacityversion")))
      {
         audacityVersion = value;
         requiredTags++;
      }

      else if (!wxStrcmp(attr, wxT("rate")))
      {
         double rate;
         Internat::CompatibleToDouble(value, &rate);
         settings.SetRate( rate );
      }

      else if (!wxStrcmp(attr, wxT("snapto")))
      {
         settings.SetSnapTo(wxString(value) == wxT("on") ? true : false);
      }

      else if (!wxStrcmp(attr, wxT("selectionformat")))
      {
         settings.SetSelectionFormat(
            NumericConverter::LookupFormat( NumericConverter::TIME, value) );
      }

      else if (!wxStrcmp(attr, wxT("audiotimeformat")))
      {
         settings.SetAudioTimeFormat(
            NumericConverter::LookupFormat( NumericConverter::TIME, value) );
      }

      else if (!wxStrcmp(attr, wxT("frequencyformat")))
      {
         settings.SetFrequencySelectionFormatName(
            NumericConverter::LookupFormat( NumericConverter::FREQUENCY, value ) );
      }

      else if (!wxStrcmp(attr, wxT("bandwidthformat")))
      {
         settings.SetBandwidthSelectionFormatName(
            NumericConverter::LookupFormat( NumericConverter::BANDWIDTH, value ) );
      }
   } // while

   if (longVpos != 0)
   {
      // PRL: It seems this must happen after SetSnapTo
      viewInfo.vpos = longVpos;
   }

   if (requiredTags < 2)
   {
      return false;
   }

   // Parse the file version from the project
   int fver;
   int frel;
   int frev;
   if (!wxSscanf(fileVersion, wxT("%i.%i.%i"), &fver, &frel, &frev))
   {
      return false;
   }

   // Parse the file version Audacity was build with
   int cver;
   int crel;
   int crev;
   wxSscanf(wxT(AUDACITY_FILE_FORMAT_VERSION), wxT("%i.%i.%i"), &cver, &crel, &crev);

   if (cver < fver || crel < frel || crev < frev)
   {
      /* i18n-hint: %s will be replaced by the version number.*/
      auto msg = XO("This file was saved using Audacity %s.\nYou are using Audacity %s. You may need to upgrade to a newer version to open this file.")
         .Format(audacityVersion, AUDACITY_VERSION_STRING);

      AudacityMessageBox(
         msg,
         XO("Can't open project file"),
         wxOK | wxICON_EXCLAMATION | wxCENTRE,
         &window);

      return false;
   }

   if (wxStrcmp(tag, wxT("project")))
   {
      return false;
   }

   // All other tests passed, so we succeed
   return true;
}

XMLTagHandler *ProjectFileIO::HandleXMLChild(const wxChar *tag)
{
   auto pProject = mpProject.lock();
   if (! pProject )
      return nullptr;
   auto &project = *pProject;
   auto fn = ProjectFileIORegistry::Lookup(tag);
   if (fn)
   {
      return fn(project);
   }

   return nullptr;
}

void ProjectFileIO::WriteXMLHeader(XMLWriter &xmlFile) const
{
   xmlFile.Write(wxT("<?xml "));
   xmlFile.Write(wxT("version=\"1.0\" "));
   xmlFile.Write(wxT("standalone=\"no\" "));
   xmlFile.Write(wxT("?>\n"));

   xmlFile.Write(wxT("<!DOCTYPE "));
   xmlFile.Write(wxT("project "));
   xmlFile.Write(wxT("PUBLIC "));
   xmlFile.Write(wxT("\"-//audacityproject-1.3.0//DTD//EN\" "));
   xmlFile.Write(wxT("\"http://audacity.sourceforge.net/xml/audacityproject-1.3.0.dtd\" "));
   xmlFile.Write(wxT(">\n"));
}

void ProjectFileIO::WriteXML(XMLWriter &xmlFile,
                             bool recording /* = false */,
                             const std::shared_ptr<TrackList> &tracks /* = nullptr */)
// may throw
{
   auto pProject = mpProject.lock();
   if (! pProject )
      THROW_INCONSISTENCY_EXCEPTION;
   auto &proj = *pProject;
   auto &tracklist = tracks ? *tracks : TrackList::Get(proj);
   auto &viewInfo = ViewInfo::Get(proj);
   auto &tags = Tags::Get(proj);
   const auto &settings = ProjectSettings::Get(proj);

   //TIMER_START( "AudacityProject::WriteXML", xml_writer_timer );

   xmlFile.StartTag(wxT("project"));
   xmlFile.WriteAttr(wxT("xmlns"), wxT("http://audacity.sourceforge.net/xml/"));

   xmlFile.WriteAttr(wxT("version"), wxT(AUDACITY_FILE_FORMAT_VERSION));
   xmlFile.WriteAttr(wxT("audacityversion"), AUDACITY_VERSION_STRING);

   viewInfo.WriteXMLAttributes(xmlFile);
   xmlFile.WriteAttr(wxT("rate"), settings.GetRate());
   xmlFile.WriteAttr(wxT("snapto"), settings.GetSnapTo() ? wxT("on") : wxT("off"));
   xmlFile.WriteAttr(wxT("selectionformat"),
                     settings.GetSelectionFormat().Internal());
   xmlFile.WriteAttr(wxT("frequencyformat"),
                     settings.GetFrequencySelectionFormatName().Internal());
   xmlFile.WriteAttr(wxT("bandwidthformat"),
                     settings.GetBandwidthSelectionFormatName().Internal());

   tags.WriteXML(xmlFile);

   unsigned int ndx = 0;
   tracklist.Any().Visit([&](Track *t)
   {
      auto useTrack = t;
      if ( recording ) {
         // When append-recording, there is a temporary "shadow" track accumulating
         // changes and displayed on the screen but it is not yet part of the
         // regular track list.  That is the one that we want to back up.
         // SubstitutePendingChangedTrack() fetches the shadow, if the track has
         // one, else it gives the same track back.
         useTrack = t->SubstitutePendingChangedTrack().get();
      }
      else if ( useTrack->GetId() == TrackId{} ) {
         // This is a track added during a non-appending recording that is
         // not yet in the undo history.  The UndoManager skips backing it up
         // when pushing.  Don't auto-save it.
         return;
      }
      useTrack->WriteXML(xmlFile);
   });

   xmlFile.EndTag(wxT("project"));

   //TIMER_STOP( xml_writer_timer );
}

bool ProjectFileIO::AutoSave(bool recording)
{
   ProjectSerializer autosave;
   WriteXMLHeader(autosave);
   WriteXML(autosave, recording);

   if (WriteDoc("autosave", autosave))
   {
      mModified = true;
      return true;
   }

   return false;
}

bool ProjectFileIO::AutoSaveDelete(sqlite3 *db /* = nullptr */)
{
   int rc;

   if (!db)
   {
      db = DB();
   }

   rc = sqlite3_exec(db, "DELETE FROM autosave;", nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Failed to remove the autosave information from the project file.")
      );
      return false;
   }

   mModified = false;

   return true;
}

bool ProjectFileIO::WriteDoc(const char *table,
                             const ProjectSerializer &autosave,
                             sqlite3 *db /* = nullptr */)
{
   int rc;

   if (!db)
   {
      db = DB();
   }

   // For now, we always use an ID of 1. This will replace the previously
   // writen row every time.
   char sql[256];
   sqlite3_snprintf(sizeof(sql),
                    sql,
                    "INSERT INTO %s(id, dict, doc) VALUES(1, ?1, ?2)"
                    "       ON CONFLICT(id) DO UPDATE SET dict = ?1, doc = ?2;",
                    table);

   sqlite3_stmt *stmt = nullptr;
   auto cleanup = finally([&]
   {
      if (stmt)
      {
         sqlite3_finalize(stmt);
      }
   });

   rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Unable to prepare project file command:\n\n%s").Format(sql)
      );
      return false;
   }

   const wxMemoryBuffer &dict = autosave.GetDict();
   const wxMemoryBuffer &data = autosave.GetData();

   // BIND SQL autosave
   // Might return SQL_MISUSE which means it's our mistake that we violated
   // preconditions; should return SQL_OK which is 0
   if (
      sqlite3_bind_blob(stmt, 1, dict.GetData(), dict.GetDataLen(), SQLITE_STATIC) ||
      sqlite3_bind_blob(stmt, 2, data.GetData(), data.GetDataLen(), SQLITE_STATIC)
   )
   {
      THROW_INCONSISTENCY_EXCEPTION;
   }

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE)
   {
      SetDBError(
         XO("Failed to update the project file.\nThe following command failed:\n\n%s").Format(sql)
      );
      return false;
   }

   return true;
}

// Importing an AUP3 project into an AUP3 project is a bit different than
// normal importing since we need to copy data from one DB to the other
// while adjusting the sample block IDs to represent the newly assigned
// IDs.
bool ProjectFileIO::ImportProject(const FilePath &fileName)
{
   // Get access to the current project file
   auto db = DB();

   bool success = false;
   bool restore = true;
   int rc;

   // Ensure the inbound database gets detached
   auto detach = finally([&]
   {
      sqlite3_exec(db, "DETACH DATABASE inbound;", nullptr, nullptr, nullptr);
   });

   // Attach the inbound project file
   wxString sql;
   sql.Printf("ATTACH DATABASE 'file:%s?immutable=1&mode=ro' AS inbound;", fileName);

   rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
   if (rc != SQLITE_OK)
   {
      SetDBError(
         XO("Unable to attach %s project file").Format(fileName)
      );

      return false;
   }

   // We need either the autosave or project docs from the inbound AUP3
   wxMemoryBuffer buffer;

   // Get the autosave doc, if any
   if (!GetBlob("SELECT dict || doc FROM inbound.project WHERE id = 1;", buffer))
   {
      // Error already set
      return false;
   }

   // If we didn't have an autosave doc, load the project doc instead
   if (buffer.GetDataLen() == 0)
   {
      if (!GetBlob("SELECT dict || doc FROM inbound.autosave WHERE id = 1;", buffer))
      {
         // Error already set
         return false;
      }

      // Missing both the autosave and project docs...this shouldn't happen!!!
      if (buffer.GetDataLen() > 0)
      {
         SetError(XO("Unable to load project or autosave documents"));
         return false;
      }
   }

   wxString project;
   BlockIDs blockids;

   // Decode it while capturing the associated sample blockids
   project = ProjectSerializer::Decode(buffer, blockids);
   if (project.size() == 0)
   {
      SetError(XO("Unable to decode project document"));

      return false;
   }

   // Parse the project doc
   wxStringInputStream in(project);
   wxXmlDocument doc;
   if (!doc.Load(in))
   {
      return false;
   }

   // Get the root ("project") node
   wxXmlNode *root = doc.GetRoot();
   wxASSERT(root->GetName().IsSameAs(wxT("project")));

   // Soft delete all non-essential attributes to prevent updating the active
   // project. This takes advantage of the knowledge that when a project is
   // parsed, unrecognized attributes are simply ignored.
   //
   // This is necessary because we don't want any of the active project settings
   // to be modified by the inbound project.
   for (wxXmlAttribute *attr = root->GetAttributes(); attr; attr = attr->GetNext())
   {
      wxString name = attr->GetName();
      if (!name.IsSameAs(wxT("version")) && !name.IsSameAs(wxT("audacityversion")))
      {
         attr->SetName(name + wxT("_deleted"));
      }
   }

   // Recursively find and collect all waveblock nodes
   std::vector<wxXmlNode *> blocknodes;
   std::function<void(wxXmlNode *)> findblocks = [&](wxXmlNode *node)
   {
      while (node)
      {
         if (node->GetName().IsSameAs(wxT("waveblock")))
         {
            blocknodes.push_back(node);
         }
         else
         {
            findblocks(node->GetChildren());
         }

         node = node->GetNext();
      }
   };

   // Get access to the active tracklist
   auto pProject = mpProject.lock();
   if (!pProject)
   {
      return false;
   }
   auto &tracklist = TrackList::Get(*pProject);

   // Search for a timetrack and remove it if the project already has one
   if (*tracklist.Any<TimeTrack>().begin())
   {
      // Find a timetrack and remove it if it exists
      for (wxXmlNode *node = doc.GetRoot()->GetChildren(); node; node = node->GetNext())
      {
         if (node->GetName().IsSameAs(wxT("timetrack")))
         {
            AudacityMessageBox(
               XO("The active project already has a time track and one was encountered in the project being imported, bypassing imported time track."),
               XO("Project Import"),
               wxOK | wxICON_EXCLAMATION | wxCENTRE,
               &GetProjectFrame(*pProject));

            root->RemoveChild(node);
            break;
         }
      }
   }

   // Find all waveblocks in all wavetracks
   for (wxXmlNode *node = doc.GetRoot()->GetChildren(); node; node = node->GetNext())
   {
      if (node->GetName().IsSameAs(wxT("wavetrack")))
      {
         findblocks(node->GetChildren());
      }
   }

   {
      // Cleanup...
      sqlite3_stmt *stmt = nullptr;
      auto cleanup = finally([&]
      {
         // Ensure the prepared statement gets cleaned up
         if (stmt)
         {
            sqlite3_finalize(stmt);
         }
      });

      // Prepare the statement to copy the sample block from the inbound project to the
      // active project.  All columns other than the blockid column gets copied.
      wxString columns(wxT("sampleformat, summin, summax, sumrms, summary256, summary64k, samples"));
      sql.Printf("INSERT INTO main.sampleblocks (%s)"
                 "   SELECT %s"
                 "   FROM inbound.sampleblocks"
                 "   WHERE blockid = ?;",
                 columns,
                 columns);

      rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
      if (rc != SQLITE_OK)
      {
         SetDBError(
            XO("Unable to prepare project file command:\n\n%s").Format(sql)
         );
         return false;
      }

      /* i18n-hint: This title appears on a dialog that indicates the progress
         in doing something.*/
      ProgressDialog progress(XO("Progress"), XO("Importing project"), pdlgHideStopButton);
      ProgressResult result = ProgressResult::Success;

      wxLongLong_t count = 0;
      wxLongLong_t total = blocknodes.size();

      // Copy all the sample blocks from the inbound project file into
      // the active one, while remembering which were copied.
      std::vector<SampleBlockID> copied;
      for (auto node : blocknodes)
      {
         // If the user cancelled the import or the import failed for some other reason
         // make sure to back out the blocks copied to the active project file
         auto backout = finally([&]
         {
            if (result == ProgressResult::Cancelled || result == ProgressResult::Failed)
            {
               for (auto blockid : copied)
               {
                  wxString sql;
                  sql.Printf("DELETE FROM main.sampleblocks WHERE blockid = %lld", blockid);

                  rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
                  if (rc != SQLITE_OK)
                  {
                     // This is non-fatal...it'll just get cleaned up the next
                     // time the project is opened.
                     SetDBError(
                        XO("Failed to delete block while cancelling import")
                     );
                  }
               }
            }
         });

         // Find the blockid attribute...it should always be there
         wxXmlAttribute *attr = node->GetAttributes();
         while (attr && !attr->GetName().IsSameAs(wxT("blockid")))
         {
            attr = attr->GetNext();
         }
         wxASSERT(attr != nullptr);

         // And get the blockid
         SampleBlockID blockid;
         attr->GetValue().ToLongLong(&blockid);

         // BIND blockid parameter
         if (sqlite3_bind_int64(stmt, 1, blockid) != SQLITE_OK)
         {
            THROW_INCONSISTENCY_EXCEPTION;
         }

         // Process it
         rc = sqlite3_step(stmt);
         if (rc != SQLITE_DONE)
         {
            SetDBError(
               XO("Failed to import sample block.\nThe following command failed:\n\n%s").Format(sql)
            );
            return false;
         }

         // Replace the original blockid with the new one
         attr->SetValue(wxString::Format(wxT("%lld"), sqlite3_last_insert_rowid(db)));

         // Reset the statement for the next iteration
         if (sqlite3_reset(stmt) != SQLITE_OK)
         {
            THROW_INCONSISTENCY_EXCEPTION;
         }

         // Remember that we copied this node in case the user cancels
         result = progress.Update(++count, total);
         if (result != ProgressResult::Success)
         {
            break;
         }
      }

      // Bail if the import was cancelled or failed. If the user stopped the
      // import or it completed, then we continue on.
      if (result == ProgressResult::Cancelled || result == ProgressResult::Failed)
      {
         return false;
      }

      // Copy over tags...likely to produce duplicates...needs work once used
      rc = sqlite3_exec(db,
                        "INSERT INTO main.tags SELECT * FROM inbound.tags;",
                        nullptr,
                        nullptr,
                        nullptr);
      if (rc != SQLITE_OK)
      {
         SetDBError(
            XO("Failed to import tags")
         );

         return false;
      }
   }

   // Recreate the project doc with the revisions we've made above
   wxStringOutputStream output;
   doc.Save(output);

   // Now load the document as normal
   XMLFileReader xmlFile;
   if (!xmlFile.ParseString(this, output.GetString()))
   {
      SetError(
         XO("Unable to parse project information.")
      );
      mLibraryError = xmlFile.GetErrorStr();

      return false;
   }

   return true;
}

bool ProjectFileIO::LoadProject(const FilePath &fileName)
{
   bool success = false;

   auto cleanup = finally([&]
   {
      if (!success)
      {
         RestoreConnection();
      }
   });

   SaveConnection();

   // Open the project file
   if (!OpenDB(fileName))
   {
      return false;
   }

   BlockIDs blockids;
   wxString project;
   wxMemoryBuffer buffer;
   bool usedAutosave = true;

   // Get the autosave doc, if any
   if (!GetBlob("SELECT dict || doc FROM autosave WHERE id = 1;", buffer))
   {
      // Error already set
      return false;
   }
 
   // If we didn't have an autosave doc, load the project doc instead
   if (buffer.GetDataLen() == 0)
   {
      usedAutosave = false;

      if (!GetBlob("SELECT dict || doc FROM project WHERE id = 1;", buffer))
      {
         // Error already set
         return false;
      }

      if (buffer.GetDataLen() == 0)
      {
         SetError(XO("Unable to load project or autosave documents"));
         return false;
      }
   }

   // Decode it while capturing the associated sample blockids
   project = ProjectSerializer::Decode(buffer, blockids);
   if (project.empty())
   {
      SetError(XO("Unable to decode project document"));

      return false;
   }

   // Check for orphans blocks...sets mRecovered if any were deleted
   if (blockids.size() > 0)
   {
      if (!CheckForOrphans(blockids))
      {
         return false;
      }
   }

   XMLFileReader xmlFile;

   // Load 'er up
   success = xmlFile.ParseString(this, project);
   if (!success)
   {
      SetError(
         XO("Unable to parse project information.")
      );
      mLibraryError = xmlFile.GetErrorStr();
      return false;
   }

   // Remember if we used autosave or not
   if (usedAutosave)
   {
      mRecovered = true;
   }

   // Mark the project modified if we recovered it
   if (mRecovered)
   {
      mModified = true;
   }

   // A previously saved project will have a document in the project table, so
   // we use that knowledge to determine if this file is an unsaved/temporary
   // file or a permanent project file
   wxString result;
   if (!GetValue("SELECT Count(*) FROM project;", result))
   {
      return false;
   }

   mTemporary = (wxStrtol<char **>(result, nullptr, 10) != 1);

   SetFileName(fileName);

   DiscardConnection();

   return true;
}

bool ProjectFileIO::SaveProject(const FilePath &fileName)
{
   wxString origName;
   bool wasTemp = false;
   bool success = false;

   // Should probably simplify all of the following by using renames.

   auto restore = finally([&]
   {
      if (!origName.empty())
      {
         if (success)
         {
            // The Save was successful, so now it is safe to abandon the
            // original connection
            DiscardConnection();
         
            // And also remove the original file if it was a temporary file
            if (wasTemp)
            {
               wxRemoveFile(origName);
            }
         }
         else
         {
            // Close the new database and go back to using the original
            // connection
            RestoreConnection();

            // And delete the new database
            wxRemoveFile(fileName);
         }
      }
   });

   // If we're saving to a different file than the current one, then copy the
   // current to the new file and make it the active file.
   if (mFileName != fileName)
   {
      // Do NOT prune here since we need to retain the Undo history
      // after we switch to the new file.
      auto newDB = CopyTo(fileName, XO("Saving project"));
      if (!newDB)
      {
         return false;
      }

      // Remember the original project filename and temporary status.  Only do
      // this after a successful copy so the "finally" block above doesn't monkey
      // with the files.
      origName = mFileName;
      wasTemp = mTemporary;

      // Save the original database connection and try to switch to a new one
      // (also ensuring closing of one of the connections, with the cooperation
      // of the finally above)
      SaveConnection();

      // Make the new connection "safe"
      Config(newDB, SafeConfig);

      // And make it the active project file 
      UseConnection(newDB, fileName);

      // Install our checkpoint hook
      sqlite3_wal_hook(mDB, CheckpointHook, this);
   }
   else
   {
      ProjectSerializer doc;
      WriteXMLHeader(doc);
      WriteXML(doc);

      if (!WriteDoc("project", doc))
      {
         return false;
      }

      // Autosave no longer needed
      AutoSaveDelete();
   }

   // Reaching this point defines success and all the rest are no-fail
   // operations:

   // No longer modified
   mModified = false;

   // No longer recovered
   mRecovered = false;

   // No longer a temporary project
   mTemporary = false;

   // Adjust the title
   SetProjectTitle();

   // Tell the finally block to behave
   success = true;

   return true;
}

bool ProjectFileIO::SaveCopy(const FilePath& fileName)
{
   auto db = CopyTo(fileName, XO("Backing up project"), true);
   if (!db)
   {
      return false;
   }

   // All good...close the database
   (void) sqlite3_close(db);

   return true;
}

bool ProjectFileIO::CloseProject()
{
   if (mDB)
   {
      // Save the filename since CloseDB() will clear it
      wxString filename = mFileName;

      // Not much we can do if this fails.  The user will simply get
      // the recovery dialog upon next restart.
      if (CloseDB())
      {
         // If this is a temporary project, we no longer want to keep the
         // project file.
         if (mTemporary)
         {
            // This is just a safety check.
            wxFileName temp(FileNames::TempDir());
            if (temp == wxPathOnly(filename))
            {
               wxRemoveFile(filename);
            }
         }
      }
   }

   return true;
}

bool ProjectFileIO::IsModified() const
{
   return mModified;
}

bool ProjectFileIO::IsTemporary() const
{
   return mTemporary;
}

bool ProjectFileIO::IsRecovered() const
{
   return mRecovered;
}

void ProjectFileIO::Reset()
{
   wxASSERT_MSG(mDB == nullptr, wxT("Resetting project with open project file"));

   mModified = false;
   mRecovered = false;

   SetFileName({});
}

wxLongLong ProjectFileIO::GetFreeDiskSpace()
{
   // make sure it's open and the path is defined
   auto db = DB();

   wxLongLong freeSpace;
   if (wxGetDiskSpace(wxPathOnly(mFileName), NULL, &freeSpace))
   {
      return freeSpace;
   }

   return -1;
}

const TranslatableString & ProjectFileIO::GetLastError() const
{
   return mLastError;
}

const TranslatableString & ProjectFileIO::GetLibraryError() const
{
   return mLibraryError;
}

void ProjectFileIO::SetError(const TranslatableString &msg)
{
   mLastError = msg;
   mLibraryError = {};
}

void ProjectFileIO::SetDBError(const TranslatableString &msg)
{
   mLastError = msg;
   wxLogDebug(wxT("SQLite error: %s"), mLastError.Debug());
   printf("   Lib error: %s", mLastError.Debug().mb_str().data());

   if (mDB)
   {
      mLibraryError = Verbatim(sqlite3_errmsg(mDB));
      wxLogDebug(wxT("   Lib error: %s"), mLibraryError.Debug());
      printf("   Lib error: %s", mLibraryError.Debug().mb_str().data());
   }
   abort();
   wxASSERT(false);
}

void ProjectFileIO::SetBypass()
{
   // Determine if we can bypass sample block deletes during shutdown.
   //
   // IMPORTANT:
   // If the project was vacuumed, then we MUST bypass further
   // deletions since the new file doesn't have the blocks that the
   // Sequences expect to be there.
   mBypass = true;

   // Only permanent project files need cleaning at shutdown
   if (!IsTemporary() && !WasVacuumed())
   {
      // If we still have unused blocks, then we must not bypass deletions
      // during shutdown.  Otherwise, we would have orphaned blocks the next time
      // the project is opened.
      //
      // An example of when dead blocks will exist is when a user opens a permanent
      // project, adds a track (with samples) to it, and chooses not to save the
      // changes.
      if (HadUnused())
      {
         mBypass = false;
      }
   }

   return;
}

bool ProjectFileIO::ShouldBypass()
{
   return mBypass;
}

AutoCommitTransaction::AutoCommitTransaction(ProjectFileIO &projectFileIO,
                                             const char *name)
:  mIO(projectFileIO),
   mName(name)
{
   mInTrans = mIO.TransactionStart(mName);
   // Must throw
}

AutoCommitTransaction::~AutoCommitTransaction()
{
   if (mInTrans)
   {
      // Can't check return status...should probably throw an exception here
      if (!Commit())
      {
         // must throw
      }
   }
}

bool AutoCommitTransaction::Commit()
{
   wxASSERT(mInTrans);

   mInTrans = !mIO.TransactionCommit(mName);

   return mInTrans;
}

bool AutoCommitTransaction::Rollback()
{
   wxASSERT(mInTrans);

   mInTrans = !mIO.TransactionCommit(mName);

   return mInTrans;
}
