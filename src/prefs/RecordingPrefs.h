/**********************************************************************

  Audacity: A Digital Audio Editor

  RecordingPrefs.h

  Joshua Haberman
  James Crook

**********************************************************************/

#ifndef __AUDACITY_RECORDING_PREFS__
#define __AUDACITY_RECORDING_PREFS__

#include <wx/defs.h>
#include <wx/textctrl.h>

#include <wx/window.h>

#include "PrefsPanel.h"

class ShuttleGui;

class RecordingPrefs :public PrefsPanel
{
 public:
   RecordingPrefs(wxWindow * parent);
   virtual ~RecordingPrefs();
   virtual bool Apply();

 private:
   void Populate();
   void PopulateOrExchange(ShuttleGui & S);
   void OnToggleCustomName(wxCommandEvent & Evt);

   wxTextCtrl *mToggleCustomName;
   bool mUseCustomTrackName;
   bool mOldNameChoice;

   DECLARE_EVENT_TABLE();
};

class RecordingPrefsFactory : public PrefsPanelFactory
{
public:
   virtual PrefsPanel *Create(wxWindow *parent);
};
#endif
