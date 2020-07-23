/**********************************************************************

Audacity: A Digital Audio Editor

DBConection.cpp

Paul Licameli -- split from ProjectFileIO.cpp

**********************************************************************/

#include "DBConnection.h"

#include "sqlite3.h"

#include <wx/progdlg.h>
#include <wx/string.h>

#include "Internat.h"
#include "Project.h"

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

DBConnection::DBConnection(const std::weak_ptr<AudacityProject> &pProject)
:  mpProject{ pProject }
{
   mDB = nullptr;
   mBypass = false;
}

DBConnection::~DBConnection()
{
   wxASSERT(mDB == nullptr);
}

void DBConnection::SetBypass( bool bypass )
{
   mBypass = bypass;
}

bool DBConnection::ShouldBypass()
{
   return mBypass;
}

bool DBConnection::Open(const char *fileName)
{
   wxASSERT(mDB == nullptr);
   int rc;

   rc = sqlite3_open(fileName, &mDB);
   if (rc != SQLITE_OK)
   {
      sqlite3_close(mDB);
      mDB = nullptr;

      return false;
   }

   // Set default mode
   SafeMode();

   // Kick off the checkpoint thread
   mCheckpointStop = false;
   mCheckpointWaitingPages = 0;
   mCheckpointCurrentPages = 0;
   mCheckpointThread = std::thread([this]{ CheckpointThread(); });

   // Install our checkpoint hook
   sqlite3_wal_hook(mDB, CheckpointHook, this);

   return mDB;
}

bool DBConnection::Close()
{
   wxASSERT(mDB != nullptr);
   int rc;

   // Protect...
   if (mDB == nullptr)
   {
      return true;
   }

   // Uninstall our checkpoint hook so that no additional checkpoints
   // are sent our way.  (Though this shouldn't really happen.)
   sqlite3_wal_hook(mDB, nullptr, nullptr);

   // Display a progress dialog if there's active or pending checkpoints
   if (mCheckpointWaitingPages || mCheckpointCurrentPages)
   {
      TranslatableString title = XO("Checkpointing project");

      // Get access to the active project
      auto project = mpProject.lock();
      if (project)
      {
         title = XO("Checkpointing %s").Format(project->GetProjectName());
      }

      // Provides a progress dialog with indeterminate mode
      wxGenericProgressDialog pd(title.Translation(),
                                 XO("This may take several seconds").Translation(),
                                 300000,     // range
                                 nullptr,    // parent
                                 wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_SMOOTH);

      // Wait for the checkpoints to end
      while (mCheckpointWaitingPages || mCheckpointCurrentPages)
      {
         wxMilliSleep(50);
         pd.Pulse();
      }
   }

   // Tell the checkpoint thread to shutdown
   {
      std::lock_guard<std::mutex> guard(mCheckpointMutex);
      mCheckpointStop = true;
      mCheckpointCondition.notify_one();
   }

   // And wait for it to do so
   mCheckpointThread.join();

   // We're done with the prepared statements
   for (auto stmt : mStatements)
   {
      sqlite3_finalize(stmt.second);
   }
   mStatements.clear();

   // Close the DB
   rc = sqlite3_close(mDB);
   if (rc != SQLITE_OK)
   {
      // I guess we could try to recover by repreparing statements and reinstalling
      // the hook, but who knows if that would work either.
      //
      // Should we throw an error???
   }

   mDB = nullptr;

   return true;
}

bool DBConnection::SafeMode(const char *schema /* = "main" */)
{
   return ModeConfig(mDB, schema, SafeConfig);
}

bool DBConnection::FastMode(const char *schema /* = "main" */)
{
   return ModeConfig(mDB, schema, FastConfig);
}

bool DBConnection::ModeConfig(sqlite3 *db, const char *schema, const char *config)
{
   // Ensure attached DB connection gets configured
   int rc;

   // Replace all schema "keywords" with the schema name
   wxString sql = config;
   sql.Replace(wxT("<schema>"), schema);

   // Set the configuration
   rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);

   return rc != SQLITE_OK;
}

sqlite3 *DBConnection::DB()
{
   wxASSERT(mDB != nullptr);

   return mDB;
}

int DBConnection::GetLastRC() const
{
   return sqlite3_errcode(mDB);
}

const wxString DBConnection::GetLastMessage() const
{
   return sqlite3_errmsg(mDB);
}

sqlite3_stmt *DBConnection::Prepare(enum StatementID id, const char *sql)
{
   int rc;

   // Return an existing statement if it's already been prepared
   auto iter = mStatements.find(id);
   if (iter != mStatements.end())
   {
      return iter->second;
   }

   // Prepare the statement
   sqlite3_stmt *stmt = nullptr;
   rc = sqlite3_prepare_v3(mDB, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, 0);
   if (rc != SQLITE_OK)
   {
      wxLogDebug("prepare error %s", sqlite3_errmsg(mDB));
      THROW_INCONSISTENCY_EXCEPTION;
   }

   // And remember it
   mStatements.insert({id, stmt});

   return stmt;
}

sqlite3_stmt *DBConnection::GetStatement(enum StatementID id)
{
   // Look it up
   auto iter = mStatements.find(id);

   // It should always be there
   wxASSERT(iter != mStatements.end());

   // Return it
   return iter->second;
}

void DBConnection::CheckpointThread()
{
   // Open another connection to the DB to prevent blocking the main thread.
   //
   // If it fails, then we won't checkpoint until the main thread closes
   // the associated DB.
   sqlite3 *db = nullptr;
   if (sqlite3_open(sqlite3_db_filename(mDB, nullptr), &db) == SQLITE_OK)
   {
      // Configure it to be safe
      ModeConfig(db, "main", SafeConfig);

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

         // And kick off the checkpoint. This may not checkpoint ALL frames
         // in the WAL.  They'll be gotten the next time around.
         sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);

         // Reset
         mCheckpointCurrentPages = 0;
      }
   }

   // All done (always close)
   sqlite3_close(db);

   return;
}

int DBConnection::CheckpointHook(void *data, sqlite3 *db, const char *schema, int pages)
{
   // Get access to our object
   DBConnection *that = static_cast<DBConnection *>(data);

   // Queue the database pointer for our checkpoint thread to process
   std::lock_guard<std::mutex> guard(that->mCheckpointMutex);
   that->mCheckpointWaitingPages = pages;
   that->mCheckpointCondition.notify_one();

   return SQLITE_OK;
}

ConnectionPtr::~ConnectionPtr() = default;

static const AudacityProject::AttachedObjects::RegisteredFactory
sConnectionPtrKey{
   []( AudacityProject & ){
      // Ignore the argument; this is just a holder of a
      // unique_ptr to DBConnection, which must be filled in later
      // (when we can get a weak_ptr to the project)
      auto result = std::make_shared< ConnectionPtr >();
      return result;
   }
};

ConnectionPtr &ConnectionPtr::Get( AudacityProject &project )
{
   auto &result =
      project.AttachedObjects::Get< ConnectionPtr >( sConnectionPtrKey );
   return result;
}

const ConnectionPtr &ConnectionPtr::Get( const AudacityProject &project )
{
   return Get( const_cast< AudacityProject & >( project ) );
}

