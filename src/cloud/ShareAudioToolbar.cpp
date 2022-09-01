/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  ShareAudioToolbar.cpp

  Dmitry Vedenko

**********************************************************************/
#include "ShareAudioToolbar.h"

#include <wx/sizer.h>
#include <wx/stattext.h>

#if wxUSE_TOOLTIPS
#include <wx/tooltip.h>
#endif

#include "AColor.h"
#include "AudioIOBase.h"
#include "AllThemeResources.h"
#include "Prefs.h"
#include "ProjectWindow.h"
#include "Theme.h"
#include "Track.h"

#include "audiocom/ShareAudioDialog.h"
#include "toolbars/ToolManager.h"
#include "widgets/AButton.h"

IMPLEMENT_CLASS(cloud::ShareAudioToolbar, ToolBar);

namespace cloud
{
ShareAudioToolbar::ShareAudioToolbar(AudacityProject& project)
    : ToolBar(project, ShareAudioBarID, XO("Share Audio"), wxT("Share Audio"))
{
}

ShareAudioToolbar::~ShareAudioToolbar()
{
}

ShareAudioToolbar& ShareAudioToolbar::Get(AudacityProject& project)
{
   auto& toolManager = ToolManager::Get(project);
   return *static_cast<ShareAudioToolbar*>(
      toolManager.GetToolBar(ShareAudioBarID));
}

const ShareAudioToolbar& ShareAudioToolbar::Get(const AudacityProject& project)
{
   return Get(const_cast<AudacityProject&>(project));
}

void ShareAudioToolbar::Create(wxWindow* parent)
{
   ToolBar::Create(parent);

   // Simulate a size event to set initial meter placement/size
   wxSizeEvent event(GetSize(), GetId());
   event.SetEventObject(this);
   GetEventHandler()->ProcessEvent(event);
}

void ShareAudioToolbar::RegenerateTooltips()
{
#if wxUSE_TOOLTIPS
   for (long iWinID = ID_SHARE_AUDIO_BUTTON; iWinID < BUTTON_COUNT; iWinID++)
   {
      auto pCtrl = static_cast<AButton*>(this->FindWindow(iWinID));
      CommandID name;
      switch (iWinID)
      {
      case ID_SHARE_AUDIO_BUTTON:
         name = wxT("Share Audio");
         break;
      }

      std::vector<ComponentInterfaceSymbol> commands(
         1u, { name, Verbatim(pCtrl->GetLabel()) });

      ToolBar::SetButtonToolTip(
         mProject, *pCtrl, commands.data(), commands.size());
   }
#endif
}

void ShareAudioToolbar::Populate()
{
   SetBackgroundColour(theTheme.Colour(clrMedium));
   MakeShareAudioButton();

#if wxUSE_TOOLTIPS
   RegenerateTooltips();
   wxToolTip::Enable(true);
   wxToolTip::SetDelay(1000);
#endif

   // Set default order and mode
   ArrangeButtons();
}

void ShareAudioToolbar::Repaint(wxDC* dc)
{
#ifndef USE_AQUA_THEME
   const auto s = mSizer->GetSize();
   const auto p = mSizer->GetPosition();

   wxRect bevelRect(p.x, p.y, s.GetWidth() - 1, s.GetHeight() - 1);
   AColor::Bevel(*dc, true, bevelRect);
#endif
}

void ShareAudioToolbar::EnableDisableButtons()
{
   auto gAudioIO = AudioIOBase::Get();

   const bool audioStreamActive = gAudioIO &&
      gAudioIO->IsStreamActive() && !gAudioIO->IsMonitoring();

   const bool hasTracks = !TrackList::Get(mProject).Any<PlayableTrack>().empty();

   mShareAudioButton->SetEnabled(hasTracks && !audioStreamActive);
}

void ShareAudioToolbar::ReCreateButtons()
{
   // ToolBar::ReCreateButtons() will get rid of its sizer and
   // since we've attached our sizer to it, ours will get deleted too
   // so clean ours up first.
   DestroySizer();

   ToolBar::ReCreateButtons();

   EnableDisableButtons();

   RegenerateTooltips();
}

void ShareAudioToolbar::MakeShareAudioButton()
{
   bool bUseAqua = false;

#ifdef EXPERIMENTAL_THEME_PREFS
   gPrefs->Read(wxT("/GUI/ShowMac"), &bUseAqua, false);
#elif defined(USE_AQUA_THEME)
   bUseAqua = true;
#endif

   const auto size = theTheme.ImageSize(bmpRecoloredSetupUpSmall);

   if (bUseAqua)
   {
      MakeMacRecoloredImageSize(
         bmpRecoloredSetupUpSmall, bmpMacUpButtonSmall, size);
      MakeMacRecoloredImageSize(
         bmpRecoloredSetupDownSmall, bmpMacDownButtonSmall, size);
      MakeMacRecoloredImageSize(
         bmpRecoloredSetupUpHiliteSmall, bmpMacHiliteUpButtonSmall, size);
      MakeMacRecoloredImageSize(
         bmpRecoloredSetupHiliteSmall, bmpMacHiliteButtonSmall, size);
   }
   else
   {
      MakeRecoloredImageSize(bmpRecoloredSetupUpSmall, bmpUpButtonSmall, size);
      MakeRecoloredImageSize(
         bmpRecoloredSetupDownSmall, bmpDownButtonSmall, size);
      MakeRecoloredImageSize(
         bmpRecoloredSetupUpHiliteSmall, bmpHiliteUpButtonSmall, size);
      MakeRecoloredImageSize(
         bmpRecoloredSetupHiliteSmall, bmpHiliteButtonSmall, size);
   }

   mShareAudioButton = MakeButton(
      this,
      bmpRecoloredSetupUpSmall, bmpRecoloredSetupDownSmall,
      bmpRecoloredSetupUpHiliteSmall, bmpRecoloredSetupHiliteSmall,
      bmpShareAudio, bmpShareAudio, bmpShareAudio,
      ID_SHARE_AUDIO_BUTTON,
      wxDefaultPosition, false, theTheme.ImageSize(bmpRecoloredSetupUpSmall));

   mShareAudioButton->SetLabel(XO("Share Audio"));

   mShareAudioButton->Bind(
      wxEVT_BUTTON,
      [this](auto)
      {
         audiocom::ShareAudioDialog dlg(mProject, &ProjectWindow::Get(mProject));
         dlg.ShowModal();

         mShareAudioButton->PopUp();
      });
}

void ShareAudioToolbar::ArrangeButtons()
{
   int flags = wxALIGN_CENTER | wxRIGHT;

   // (Re)allocate the button sizer
   DestroySizer();

   Add((mSizer = safenew wxBoxSizer(wxHORIZONTAL)), 1, wxEXPAND);

   auto text = safenew wxStaticText(this, wxID_ANY, XO("Share Audio").Translation());
   text->SetBackgroundColour(theTheme.Colour(clrMedium));
   text->SetForegroundColour(theTheme.Colour(clrTrackPanelText));

   auto vSizer = safenew wxBoxSizer(wxVERTICAL);
   vSizer->AddSpacer(4);
   vSizer->Add(mShareAudioButton, 0, flags, 2);
   vSizer->AddSpacer(4);
   vSizer->Add(text, 0, flags, 2);

   // Start with a little extra space
   mSizer->Add(5, 55);
   mSizer->Add(vSizer, 1, wxEXPAND);
   mSizer->Add(5, 55);

   // Layout the sizer
   mSizer->Layout();

   // Layout the toolbar
   Layout();

   SetMinSize(GetSizer()->GetMinSize());
}

void ShareAudioToolbar::DestroySizer()
{
   if (mSizer == nullptr)
      return;

   Detach(mSizer);

   std::unique_ptr<wxSizer> { mSizer }; // DELETE it
   mSizer = nullptr;
}

static RegisteredToolbarFactory factory {
   ShareAudioBarID, [](AudacityProject& project)
   { return ToolBar::Holder { safenew ShareAudioToolbar { project } }; }
};
} // namespace cloud
