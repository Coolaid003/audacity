/**********************************************************************

  Audacity: A Digital Audio Editor

  MacroCommands.cpp

  Dominic Mazzoni
  James Crook

********************************************************************//*!

\class MacroCommands
\brief Maintains the chain of commands used in batch processing.
See also MacroCommandDialog and ApplyMacroDialog.

*//*******************************************************************/


#include "Audacity.h"
#include "BatchCommands.h"

#include <wx/defs.h>
#include <wx/dir.h>
#include <wx/filedlg.h>
#include <wx/textfile.h>

#include "AudacityApp.h"
#include "Project.h"
#include "commands/CommandManager.h"
#include "effects/EffectManager.h"
#include "FileNames.h"
#include "Internat.h"
#include "PluginManager.h"
#include "Prefs.h"
#include "Shuttle.h"
#include "export/ExportFLAC.h"
#include "export/ExportMP3.h"
#include "export/ExportOGG.h"
#include "export/ExportPCM.h"

#include "Theme.h"
#include "AllThemeResources.h"

#include "Track.h"
#include "widgets/ErrorDialog.h"

#include "commands/CommandFunctors.h"
#include "commands/CommandContext.h"

// KLUDGE: All commands should be on the same footing
// however, for historical reasons we distinguish between
//    - Effects (which are looked up in effects lists)
//    - Menu commands (which are held in command manager)
//    - Specials (which we deal with specially here)
enum eCommandType { CtEffect, CtMenu, CtSpecial };

// TIDY-ME: Not currently translated,
// but there are issues to address if we do.
// CLEANSPEECH remnant
static const std::pair<const wxChar*, const wxChar*> SpecialCommands[] = {
   // Use translations of the first members, some other day.
   // For 2.2.2 we'll get them into the catalog at least.

   { XO("No Action"),            wxT("NoAction") },

   // { wxT("Import"), wxT("Import") },   // non-functioning
   /* i18n-hint: before is adverb; MP3 names an audio file format */
   { XO("Export as MP3 56k before"), wxT("ExportMP3_56k_before") },

   /* i18n-hint: after is adverb; MP3 names an audio file format */
   { XO("Export as MP3 56k after"),  wxT("ExportMP3_56k_after") },

   /* i18n-hint: FLAC names an audio file format */
   { XO("Export as FLAC"),          wxT("ExportFLAC") },

// MP3 OGG and WAV already handled by menu items.
#if 0
   /* i18n-hint: MP3 names an audio file format */
   { XO("Export as MP3"),           wxT("ExportMP3") },

   /* i18n-hint: Ogg names an audio file format */
   { XO("Export as Ogg"),           wxT("ExportOgg") },

   /* i18n-hint: WAV names an audio file format */
   { XO("Export as WAV"),           wxT("ExportWAV") },
#endif
};
// end CLEANSPEECH remnant

MacroCommands::MacroCommands()
{
   mMessage = "";
   ResetMacro();

   wxArrayString names = GetNames();
   wxArrayString defaults = GetNamesOfDefaultMacros();

   for( size_t i = 0;i<defaults.Count();i++){
      wxString name = defaults[i];
      if (names.Index(name) == wxNOT_FOUND) {
         AddMacro(name);
         RestoreMacro(name);
         WriteMacro(name);
      }
   }
}

static const wxString MP3Conversion = wxT("MP3 Conversion");
static const wxString FadeEnds      = wxT("Fade Ends");


wxArrayString MacroCommands::GetNamesOfDefaultMacros()
{
   wxArrayString defaults;
   defaults.Add( MP3Conversion );
   defaults.Add( FadeEnds  );
   return defaults;
}

void MacroCommands::RestoreMacro(const wxString & name)
{
// TIDY-ME: Effects change their name with localisation.
// Commands (at least currently) don't.  Messy.
   ResetMacro();
   if (name == MP3Conversion){
        AddToMacro( wxT("Normalize") );
        AddToMacro( wxT("ExportMP3") );
   } else if (name == FadeEnds ){
        AddToMacro( wxT("Select"), wxT("Start=\"0\" End=\"1\"") );
        AddToMacro( wxT("FadeIn") );
        AddToMacro( wxT("Select"), wxT("Start=\"0\" End=\"1\" FromEnd=\"1\"") );
        AddToMacro( wxT("FadeOut") );
        AddToMacro( wxT("Select"), wxT("Start=\"0\" End=\"0\"") );
   }
}

wxString MacroCommands::GetCommand(int index)
{
   if (index < 0 || index >= (int)mCommandMacro.GetCount()) {
      return wxT("");
   }

   return mCommandMacro[index];
}

wxString MacroCommands::GetParams(int index)
{
   if (index < 0 || index >= (int)mParamsMacro.GetCount()) {
      return wxT("");
   }

   return mParamsMacro[index];
}

int MacroCommands::GetCount()
{
   return (int)mCommandMacro.GetCount();
}

bool MacroCommands::ReadMacro(const wxString & chain)
{
   // Clear any previous chain
   ResetMacro();

   // Build the filename
   wxFileName name(FileNames::MacroDir(), chain, wxT("txt"));

   // Set the file name
   wxTextFile tf(name.GetFullPath());

   // Open and check
   tf.Open();
   if (!tf.IsOpened()) {
      // wxTextFile will display any errors
      return false;
   }

   // Load commands from the file
   int lines = tf.GetLineCount();
   if (lines > 0) {
      for (int i = 0; i < lines; i++) {

         // Find the command name terminator...ingore line if not found
         int splitAt = tf[i].Find(wxT(':'));
         if (splitAt < 0) {
            continue;
         }

         // Parse and clean
         wxString cmd = tf[i].Left(splitAt).Strip(wxString::both);
         wxString parm = tf[i].Mid(splitAt + 1).Strip(wxString::trailing);

         // Add to lists
         mCommandMacro.Add(cmd);
         mParamsMacro.Add(parm);
      }
   }

   // Done with the file
   tf.Close();

   return true;
}


bool MacroCommands::WriteMacro(const wxString & chain)
{
   // Build the filename
   wxFileName name(FileNames::MacroDir(), chain, wxT("txt"));

   // Set the file name
   wxTextFile tf(name.GetFullPath());

   // Create the file (Create() doesn't leave the file open)
   if (!tf.Exists()) {
      tf.Create();
   }

   // Open it
   tf.Open();

   if (!tf.IsOpened()) {
      // wxTextFile will display any errors
      return false;
   }

   // Start with a clean slate
   tf.Clear();

   // Copy over the commands
   int lines = mCommandMacro.GetCount();
   for (int i = 0; i < lines; i++) {
      tf.AddLine(mCommandMacro[i] + wxT(":") + mParamsMacro[ i ]);
   }

   // Write the chain
   tf.Write();

   // Done with the file
   tf.Close();

   return true;
}

bool MacroCommands::AddMacro(const wxString & chain)
{
   // Build the filename
   wxFileName name(FileNames::MacroDir(), chain, wxT("txt"));

   // Set the file name
   wxTextFile tf(name.GetFullPath());

   // Create it..Create will display errors
   return tf.Create();
}

bool MacroCommands::DeleteMacro(const wxString & chain)
{
   // Build the filename
   wxFileName name(FileNames::MacroDir(), chain, wxT("txt"));

   // Delete it...wxRemoveFile will display errors
   return wxRemoveFile(name.GetFullPath());
}

bool MacroCommands::RenameMacro(const wxString & oldchain, const wxString & newchain)
{
   // Build the filenames
   wxFileName oname(FileNames::MacroDir(), oldchain, wxT("txt"));
   wxFileName nname(FileNames::MacroDir(), newchain, wxT("txt"));

   // Rename it...wxRenameFile will display errors
   return wxRenameFile(oname.GetFullPath(), nname.GetFullPath());
}

// Gets all commands that are valid for this mode.
auto MacroCommands::GetAllCommands() -> CommandNameVector
{
   CommandNameVector commands;

   AudacityProject *project = GetActiveProject();
   if (!project)
      return commands;

   // CLEANSPEECH remnant
   for( const auto &command : SpecialCommands )
      commands.push_back(
         CommandName( command.first, command.second, _("Special Command") )
      );

   // end CLEANSPEECH remnant

   PluginManager & pm = PluginManager::Get();
   EffectManager & em = EffectManager::Get();
   {
      const PluginDescriptor *plug = pm.GetFirstPlugin(PluginTypeEffect|PluginTypeAudacityCommand);
      while (plug)
      {
         auto command = em.GetCommandIdentifier(plug->GetID());
         if (!command.IsEmpty())
            commands.push_back( 
               CommandName( 
                  plug->GetUntranslatedName(), // plug->GetTranslatedName(),
                  command,
                  plug->GetPluginType() == PluginTypeEffect ?
                     _("Effect") : _("Menu Command (With Parameters)") 
               )
            );
         plug = pm.GetNextPlugin(PluginTypeEffect|PluginTypeAudacityCommand);
      }
   }

   CommandManager * mManager = project->GetCommandManager();
   wxArrayString mLabels;
   wxArrayString mNames;
   mLabels.Clear();
   mNames.Clear();
   mManager->GetAllCommandLabels(mLabels, false);
   mManager->GetAllCommandNames(mNames, false);
   for(size_t i=0; i<mNames.GetCount(); i++) {
      wxString label = mLabels[i];
      if( !label.Contains( "..." ) ){
         label.Replace( "&", "" );
         wxString squashed = label;
         squashed.Replace( " ", "" );

         // We'll disambiguate if the squashed name is short and shorter than the internal name.
         // Otherwise not.
         // This means we won't have repetitive items like "Cut (Cut)" 
         // But we will show important disambiguation like "All (SelectAll)" and "By Date (SortByDate)"
         // Disambiguation is no longer essential as the details box will show it.
         if( squashed.Length() < wxMin( 18, mNames[i].Length()) )
            label = label + " (" + mNames[i] + ")";

         commands.push_back( 
            CommandName(
               label, // User readable name 
               mNames[i], // Internal name.
               _("Menu Command (No Parameters)")
            )
         );
      }
   }

   // Sort commands by their user-visible names.
   // PRL:  What should happen if first members of pairs are not unique?
   // Sort stably?
   std::sort(
      commands.begin(), commands.end(),
      [](const CommandName &a, const CommandName &b)
         { return std::get<0>(a) <  std::get<0>(b); }
   );

   // JKC: Gave up on trying to use std::unique on this.
   CommandNameVector uniqueCommands;
   unsigned size = commands.size();
   wxString oldName = "";
   for( unsigned i = 0; i < size; ++i ) 
   {
      if( std::get<0>( commands[i] ) != oldName )
         uniqueCommands.push_back( commands[i] );
      oldName = std::get<0>( commands[i] );
   }
   return uniqueCommands;
}

wxString MacroCommands::GetCurrentParamsFor(const wxString & command)
{
   const PluginID & ID = EffectManager::Get().GetEffectByIdentifier(command);
   if (ID.empty())
   {
      return wxEmptyString;   // effect not found.
   }

   return EffectManager::Get().GetEffectParameters(ID);
}

wxString MacroCommands::PromptForParamsFor(const wxString & command, const wxString & params, wxWindow *parent)
{
   const PluginID & ID = EffectManager::Get().GetEffectByIdentifier(command);
   if (ID.empty())
   {
      return wxEmptyString;   // effect not found
   }

   wxString res = params;

   auto cleanup = EffectManager::Get().SetBatchProcessing(ID);

   if (EffectManager::Get().SetEffectParameters(ID, params))
   {
      if (EffectManager::Get().PromptUser(ID, parent))
      {
         res = EffectManager::Get().GetEffectParameters(ID);
      }
   }

   return res;
}

wxString MacroCommands::PromptForPresetFor(const wxString & command, const wxString & params, wxWindow *parent)
{
   const PluginID & ID = EffectManager::Get().GetEffectByIdentifier(command);
   if (ID.empty())
   {
      return wxEmptyString;   // effect not found.
   }

   wxString preset = EffectManager::Get().GetPreset(ID, params, parent);

   // Preset will be empty if the user cancelled the dialog, so return the original
   // parameter value.
   if (preset.IsEmpty())
   {
      return params;
   }

   return preset;
}

double MacroCommands::GetEndTime()
{
   AudacityProject *project = GetActiveProject();
   if( project == NULL )
   {
      //AudacityMessageBox( _("No project to process!") );
      return -1.0;
   }
   TrackList * tracks = project->GetTracks();
   if( tracks == NULL )
   {
      //AudacityMessageBox( _("No tracks to process!") );
      return -1.0;
   }

   double endTime = tracks->GetEndTime();
   return endTime;
}

bool MacroCommands::IsMono()
{
   AudacityProject *project = GetActiveProject();
   if( project == NULL )
   {
      //AudacityMessageBox( _("No project and no Audio to process!") );
      return false;
   }

   TrackList * tracks = project->GetTracks();
   if( tracks == NULL )
   {
      //AudacityMessageBox( _("No tracks to process!") );
      return false;
   }

   TrackListIterator iter(tracks);
   Track *t = iter.First();
   bool mono = true;
   while (t) {
      if (t->GetLinked()) {
         mono = false;
         break;
      }
      t = iter.Next();
   }

   return mono;
}

wxString MacroCommands::BuildCleanFileName(const wxString &fileName, const wxString &extension)
{
   const wxFileName newFileName{ fileName };
   wxString justName = newFileName.GetName();
   wxString pathName = newFileName.GetPath(wxPATH_GET_VOLUME | wxPATH_GET_SEPARATOR);
   const auto cleanedString = _("cleaned");

   if (justName.empty()) {
      wxDateTime now = wxDateTime::Now();
      int year = now.GetYear();
      wxDateTime::Month month = now.GetMonth();
      wxString monthName = now.GetMonthName(month);
      int dom = now.GetDay();
      int hour = now.GetHour();
      int minute = now.GetMinute();
      int second = now.GetSecond();
      justName = wxString::Format(wxT("%d-%s-%02d-%02d-%02d-%02d"),
           year, monthName, dom, hour, minute, second);

//      SetName(cleanedFileName);
//      bool isStereo;
//      double endTime = project->mTracks->GetEndTime();
//      double startTime = 0.0;
      //OnSelectAll();
      pathName = FileNames::FindDefaultPath(FileNames::Operation::Export);
      ::AudacityMessageBox(wxString::Format(_("Export recording to %s\n/%s/%s%s"),
            pathName, cleanedString, justName, extension),
         _("Export recording"),
         wxOK | wxCENTRE);
      pathName += wxFileName::GetPathSeparator();
   }
   wxString cleanedName = pathName;
   cleanedName += cleanedString;
   bool flag  = ::wxFileName::FileExists(cleanedName);
   if (flag == true) {
      ::AudacityMessageBox(_("Cannot create directory 'cleaned'. \nFile already exists that is not a directory"));
      return wxString{};
   }
   ::wxFileName::Mkdir(cleanedName, 0777, wxPATH_MKDIR_FULL); // make sure it exists

   cleanedName += wxFileName::GetPathSeparator();
   cleanedName += justName;
   cleanedName += extension;
   wxGetApp().AddFileToHistory(cleanedName);

   return cleanedName;
}

// TODO Move this out of Batch Commands
bool MacroCommands::WriteMp3File( const wxString & Name, int bitrate )
{  //check if current project is mono or stereo
   unsigned numChannels = 2;
   if (IsMono()) {
      numChannels = 1;
   }

   double endTime = GetEndTime();
   if( endTime <= 0.0f )
      return false;
   AudacityProject *project = GetActiveProject();
   if( bitrate <=0 )
   {
      // 'No' bitrate given, use the current default.
      // Use Mp3Stereo to control if export is to a stereo or mono file
      return mExporter.Process(project, numChannels, wxT("MP3"), Name, false, 0.0, endTime);
   }


   bool rc;
   long prevBitRate = gPrefs->Read(wxT("/FileFormats/MP3Bitrate"), 128);
   gPrefs->Write(wxT("/FileFormats/MP3Bitrate"), bitrate);

   auto cleanup = finally( [&] {
      gPrefs->Write(wxT("/FileFormats/MP3Bitrate"), prevBitRate);
      gPrefs->Flush();
   } );

   // Use Mp3Stereo to control if export is to a stereo or mono file
   rc = mExporter.Process(project, numChannels, wxT("MP3"), Name, false, 0.0, endTime);
   return rc;
}

// TIDY-ME: Get rid of special commands and make them part of the
// 'menu' system (but not showing on the menu)
//
// ======= IMPORTANT ========
// Special Commands are a KLUDGE whilst we wait for a better system to handle the menu
// commands from batch mode.
//
// Really we should be using a similar (or same) system to that used for effects
// so that parameters can be passed to the commands.  Many of the menu
// commands take a selection as their parameter.
//
// If you find yourself adding lots of existing commands from the menus here, STOP
// and think again.
// ======= IMPORTANT ========
// CLEANSPEECH remnant
bool MacroCommands::ApplySpecialCommand(int WXUNUSED(iCommand), const wxString & command,const wxString & params)
{
   if (ReportAndSkip(command, params))
      return true;

   AudacityProject *project = GetActiveProject();

   unsigned numChannels = 1;    //used to switch between mono and stereo export
   if (IsMono()) {
      numChannels = 1;  //export in mono
   } else {
      numChannels = 2;  //export in stereo
   }

   wxString filename;
   wxString extension; // required for correct message
   if (command == wxT("ExportWAV"))
      extension = wxT(".wav");
   else if (command == wxT("ExportOgg"))
      extension = wxT(".ogg");
   else if (command == wxT("ExportFLAC"))
      extension = wxT(".flac");
   else extension = wxT(".mp3");

   if (mFileName.IsEmpty()) {
      filename = BuildCleanFileName(project->GetFileName(), extension);
   }
   else {
      filename = BuildCleanFileName(mFileName, extension);
   }

   // We have a command index, but we don't use it!
   // TODO: Make this special-batch-command code use the menu item code....
   // FIXME: TRAP_ERR No error reporting on write file failure in batch mode.
   if (command == wxT("NoAction")) {
      return true;
   } else if (!mFileName.IsEmpty() && command == wxT("Import")) {
      // historically this was in use, now ignored if there
      return true;
   } else if (command == wxT("ExportMP3_56k_before")) {
      filename.Replace(wxT("cleaned/"), wxT("cleaned/MasterBefore_"), false);
      return WriteMp3File(filename, 56);
   } else if (command == wxT("ExportMP3_56k_after")) {
      filename.Replace(wxT("cleaned/"), wxT("cleaned/MasterAfter_"), false);
      return WriteMp3File(filename, 56);
   } else if (command == wxT("ExportMP3")) {
      return WriteMp3File(filename, 0); // 0 bitrate means use default/current
   } else if (command == wxT("ExportWAV")) {
      filename.Replace(wxT(".mp3"), wxT(".wav"), false);
      double endTime = GetEndTime();
      if (endTime <= 0.0f) {
         return false;
      }
      return mExporter.Process(project, numChannels, wxT("WAV"), filename, false, 0.0, endTime);
   } else if (command == wxT("ExportOgg")) {
#ifdef USE_LIBVORBIS
      filename.Replace(wxT(".mp3"), wxT(".ogg"), false);
      double endTime = GetEndTime();
      if (endTime <= 0.0f) {
         return false;
      }
      return mExporter.Process(project, numChannels, wxT("OGG"), filename, false, 0.0, endTime);
#else
      AudacityMessageBox(_("Ogg Vorbis support is not included in this build of Audacity"));
      return false;
#endif
   } else if (command == wxT("ExportFLAC")) {
#ifdef USE_LIBFLAC
      filename.Replace(wxT(".mp3"), wxT(".flac"), false);
      double endTime = GetEndTime();
      if (endTime <= 0.0f) {
         return false;
      }
      return mExporter.Process(project, numChannels, wxT("FLAC"), filename, false, 0.0, endTime);
#else
      AudacityMessageBox(_("FLAC support is not included in this build of Audacity"));
      return false;
#endif
   }
   AudacityMessageBox(wxString::Format(_("Command %s not implemented yet"),command));
   return false;
}
// end CLEANSPEECH remnant

bool MacroCommands::ApplyEffectCommand(const PluginID & ID, const wxString & command, const wxString & params, const CommandContext & Context)
{
   //Possibly end processing here, if in batch-debug
   if( ReportAndSkip(command, params))
      return true;

   const PluginDescriptor *plug = PluginManager::Get().GetPlugin(ID);
   if (!plug)
      return false;

   AudacityProject *project = GetActiveProject();

   // FIXME: for later versions may want to not select-all in batch mode.
   // IF nothing selected, THEN select everything
   // (most effects require that you have something selected).
   if( plug->GetPluginType() != PluginTypeAudacityCommand )
      project->SelectAllIfNone();

   bool res = false;

   auto cleanup = EffectManager::Get().SetBatchProcessing(ID);

   // transfer the parameters to the effect...
   if (EffectManager::Get().SetEffectParameters(ID, params))
   {
      if( plug->GetPluginType() == PluginTypeAudacityCommand )
         // and apply the effect...
         res = project->DoAudacityCommand(ID, 
            Context,
            AudacityProject::OnEffectFlags::kConfigured |
            AudacityProject::OnEffectFlags::kSkipState |
            AudacityProject::OnEffectFlags::kDontRepeatLast);
      else
         // and apply the effect...
         res = project->DoEffect(ID, 
            Context,
            AudacityProject::OnEffectFlags::kConfigured |
            AudacityProject::OnEffectFlags::kSkipState |
            AudacityProject::OnEffectFlags::kDontRepeatLast);
   }

   return res;
}

bool MacroCommands::ApplyCommand(const wxString & command, const wxString & params, CommandContext const * pContext)
{

   unsigned int i;
   // Test for a special command.
   // CLEANSPEECH remnant
   for( i = 0; i < sizeof(SpecialCommands)/sizeof(*SpecialCommands); ++i ) {
      if( command.IsSameAs( SpecialCommands[i].second, false) )
         return ApplySpecialCommand( i, command, params );
   }
   // end CLEANSPEECH remnant

   // Test for an effect.
   const PluginID & ID = EffectManager::Get().GetEffectByIdentifier( command );
   if (!ID.empty())
   {
      if( pContext )
         return ApplyEffectCommand(ID, command, params, *pContext);
      const CommandContext context(  *GetActiveProject() );
      return ApplyEffectCommand(ID, command, params, context);
   }

   AudacityProject *project = GetActiveProject();
   CommandManager * pManager = project->GetCommandManager();
   if( pContext ){
      if( pManager->HandleTextualCommand( command, *pContext, AlwaysEnabledFlag, AlwaysEnabledFlag ) )
         return true;
      pContext->Status( wxString::Format(
         _("Your batch command of %s was not recognized."), command ));
      return false;
   }
   else
   {
      const CommandContext context(  *GetActiveProject() );
      if( pManager->HandleTextualCommand( command, context, AlwaysEnabledFlag, AlwaysEnabledFlag ) )
         return true;
   }

   AudacityMessageBox(
      wxString::Format(
      _("Your batch command of %s was not recognized."), command ));

   return false;
}

bool MacroCommands::ApplyCommandInBatchMode(const wxString & command, const wxString &params)
{
   AudacityProject *project = GetActiveProject();

   // enter batch mode...
   bool prevShowMode = project->GetShowId3Dialog();
   auto cleanup = finally( [&] {
      // exit batch mode...
      project->SetShowId3Dialog(prevShowMode);
   } );

   return ApplyCommand( command, params );
}

static int MacroReentryCount = 0;
// ApplyMacro returns true on success, false otherwise.
// Any error reporting to the user in setting up the chain
// has already been done.
bool MacroCommands::ApplyMacro(const wxString & filename)
{
   // Check for reentrant ApplyMacro commands.
   // We'll allow 1 level of reentry, but not more.
   // And we treat ignoring deeper levels as a success.
   if( MacroReentryCount > 1 )
      return true;

   // Restore the reentry counter (to zero) when we exit.
   auto cleanup1 = valueRestorer( MacroReentryCount);
   MacroReentryCount++;

   mFileName = filename;

   AudacityProject *proj = GetActiveProject();
   bool res = false;
   auto cleanup2 = finally( [&] {
      if (!res) {
         if(proj) {
            // Macro failed or was cancelled; revert to the previous state
            proj->RollbackState();
         }
      }
   } );

   mAbort = false;

   size_t i = 0;
   for (; i < mCommandMacro.GetCount(); i++) {
      if (!ApplyCommandInBatchMode(mCommandMacro[i], mParamsMacro[i]) || mAbort)
         break;
   }

   res = (i == mCommandMacro.GetCount());
   if (!res)
      return false;

   mFileName.Empty();

   // Macro was successfully applied; save the NEW project state
   wxString longDesc, shortDesc;
   wxString name = gPrefs->Read(wxT("/Batch/ActiveMacro"), wxEmptyString);
   if (name.IsEmpty())
   {
      /* i18n-hint: active verb in past tense */
      longDesc = _("Applied batch chain");
      shortDesc = _("Apply chain");
   }
   else
   {
      /* i18n-hint: active verb in past tense */
      longDesc = wxString::Format(_("Applied batch chain '%s'"), name);
      shortDesc = wxString::Format(_("Apply '%s'"), name);
   }

   if (!proj)
      return false;
   if( MacroReentryCount <= 1 )
      proj->PushState(longDesc, shortDesc);
   return true;
}

// AbortBatch() allows a premature terminatation of a batch.
void MacroCommands::AbortBatch()
{
   mAbort = true;
}

void MacroCommands::AddToMacro(const wxString &command, int before)
{
   AddToMacro(command, GetCurrentParamsFor(command), before);
}

void MacroCommands::AddToMacro(const wxString &command, const wxString &params, int before)
{
   if (before == -1) {
      before = (int)mCommandMacro.GetCount();
   }

   mCommandMacro.Insert(command, before);
   mParamsMacro.Insert(params, before);
}

void MacroCommands::DeleteFromMacro(int index)
{
   if (index < 0 || index >= (int)mCommandMacro.GetCount()) {
      return;
   }

   mCommandMacro.RemoveAt(index);
   mParamsMacro.RemoveAt(index);
}

void MacroCommands::ResetMacro()
{
   mCommandMacro.Clear();
   mParamsMacro.Clear();
}

// ReportAndSkip() is a diagnostic function that avoids actually
// applying the requested effect if in batch-debug mode.
bool MacroCommands::ReportAndSkip(const wxString & command, const wxString & params)
{
   int bDebug;
   gPrefs->Read(wxT("/Batch/Debug"), &bDebug, false);
   if( bDebug == 0 )
      return false;

   //TODO: Add a cancel button to these, and add the logic so that we can abort.
   if( params != wxT("") )
   {
      AudacityMessageBox( wxString::Format(_("Apply %s with parameter(s)\n\n%s"),command, params),
         _("Test Mode"));
   }
   else
   {
      AudacityMessageBox( wxString::Format(_("Apply %s"),command),
         _("Test Mode"));
   }
   return true;
}

wxArrayString MacroCommands::GetNames()
{
   wxArrayString names;
   wxArrayString files;
   wxDir::GetAllFiles(FileNames::MacroDir(), &files, wxT("*.txt"), wxDIR_FILES);
   size_t i;

   wxFileName ff;
   for (i = 0; i < files.GetCount(); i++) {
      ff = (files[i]);
      names.Add(ff.GetName());
   }

   return names;
}

bool MacroCommands::IsFixed(const wxString & name)
{
   wxArrayString defaults = GetNamesOfDefaultMacros();
   if( defaults.Index( name ) != wxNOT_FOUND )
      return true;
   return false;
}

void MacroCommands::Split(const wxString & str, wxString & command, wxString & param)
{
   int splitAt;

   command.Empty();
   param.Empty();

   if (str.IsEmpty()) {
      return;
   }

   splitAt = str.Find(wxT(':'));
   if (splitAt < 0) {
      return;
   }

   command = str.Mid(0, splitAt);
   param = str.Mid(splitAt + 1);

   return;
}

wxString MacroCommands::Join(const wxString & command, const wxString & param)
{
   return command + wxT(": ") + param;
}
