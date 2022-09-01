/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  ShareAudioDialog.cpp

  Dmitry Vedenko

**********************************************************************/
#include "ShareAudioDialog.h"

#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/gauge.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/textctrl.h>

#include "AllThemeResources.h"
#include "BasicUI.h"
#include "MemoryX.h"
#include "Project.h"
#include "ShuttleGui.h"
#include "Theme.h"
#include "Track.h"
#include "TempDirectory.h"

#include "ServiceConfig.h"
#include "OAuthService.h"
#include "UploadService.h"
#include "UserService.h"

#include "LinkAccountDialog.h"
#include "UserImage.h"

#include "CodeConversions.h"

#include "export/Export.h"
#include "ui/AccessibleLinksFormatter.h"

#include "widgets/HelpSystem.h"

#ifdef HAS_CUSTOM_URL_HANDLING
#include "URLSchemesRegistry.h"
#endif

namespace cloud::audiocom
{
namespace
{
BoolSetting wasOpened { L"/cloud/audiocom/wasOpened", false };

const wxSize avatarSize = { 32, 32 };

using ExportHelper = ExportPlugin;

bool GenerateTempPath(ExportHelper &helper, wxString &path)
{
   FilePath pathName = TempDirectory::DefaultTempDir();

   if (!FileNames::WritableLocationCheck(
          pathName, XO("Cannot proceed to export.")))
   {
      return false;
   }

   wxFileName fileName(
      pathName + "/cloud/",
      wxString::Format("%lld", std::chrono::system_clock::now().time_since_epoch().count()),
      helper.GetUploadExtension());

   fileName.Mkdir(0700, wxPATH_MKDIR_FULL);

   if (fileName.Exists())
   {
      if (!wxRemoveFile(path))
         return false;
   }

   path = fileName.GetFullPath();
   return true;
}

bool DoExport(AudacityProject &project, Exporter &e,
   ExportHelper &helper, std::unique_ptr<BasicUI::ProgressDialog>& progress,
   /* out */ wxString &path)
{
   if (!GenerateTempPath(helper, path))
      return false;

   SettingScope scope;

   helper.SetupUploadFormat();

   auto& tracks = TrackList::Get(project);

   const double t0 = 0.0;
   const double t1 = tracks.GetEndTime();

   const int nChannels = (tracks.Any() - &Track::IsLeader).empty() ? 1 : 2;

   return e.Process(
      nChannels,   // numChannels,
      helper.GetUploadFormat(), // type,
      path,       // full path,
      false,       // selectedOnly,
      t0,          // t0
      t1,          // t1
      progress     // progress dialog
   );
}

namespace {
Identifier GetPreferredAudioFormat()
{
   // Identifier that a plug-in was registered with -- though this makes
   // a coincidence of string literals.  Should this become a StringSetting?
   return "WavPack";
}
}

ExportHelper *GetUploadHelper(Exporter &e)
{
   const auto preferredFormat = GetPreferredAudioFormat();
   size_t ii = 0;
   for (auto &id : e.GetPluginIDs()) {
      if (id == preferredFormat)
         return e.GetPlugins()[ii].get();
      ++ii;
   }
   return nullptr;
}
}

// A helper structures holds UploadService and UploadPromise
struct ShareAudioDialog::Services final
{
   UploadService uploadService;

   UploadOperationHandle uploadPromise;

   Services()
       : uploadService(GetServiceConfig(), GetOAuthService())
   {
   }
};

// Implementation of the ProgressDialog, which is not a dialog.
// Instead, progress is forwarded to the parent
struct ShareAudioDialog::ExportProgressHelper final :
    public BasicUI::ProgressDialog
{
   explicit ExportProgressHelper(ShareAudioDialog& parent)
       : mParent(parent)
   {
   }

   void Cancel()
   {
      mCancelled = true;
   }

   bool WasCancelled()
   {
      return mCancelled;
   }

   BasicUI::ProgressResult Poll(
      unsigned long long numerator, unsigned long long denominator,
      const TranslatableString&) override
   {
      mParent.UpdateProgress(numerator, denominator);

      const auto now = Clock::now();

      // Exporter polls in the main thread. To make the dialog responsive
      // periodic yielding is required
      if ((now - mLastYield > std::chrono::milliseconds(50)) || (numerator == denominator))
      {
         BasicUI::Yield();
         mLastYield = now;
      }

      return mCancelled ? BasicUI::ProgressResult::Cancelled :
                          BasicUI::ProgressResult::Success;
   }

   void SetMessage(const TranslatableString&) override
   {
   }

   void SetDialogTitle(const TranslatableString&) override
   {
   }

   void Reinit() override
   {
   }

   ShareAudioDialog& mParent;

   using Clock = std::chrono::steady_clock;
   Clock::time_point mLastYield;

   bool mCancelled { false };
};

ShareAudioDialog::ShareAudioDialog(AudacityProject& project, wxWindow* parent)
    : wxDialogWrapper(
         parent, wxID_ANY, XO("Share Audio"), wxDefaultPosition, { 480, 250 },
         wxDEFAULT_DIALOG_STYLE)
    , mProject(project)
    , mServices(std::make_unique<Services>())
{
   ShuttleGui s(this, eIsCreating);

   s.StartVerticalLay();
   {
      Populate(s);
   }
   s.EndVerticalLay();

   Layout();
   Fit();
   Centre();

   SetMinSize(GetSize());
   SetMaxSize({ GetSize().x, -1 });

   mContinueAction = [this]()
   {
      if (mInitialStatePanel.root->IsShown())
         StartUploadProcess();
   };
}

ShareAudioDialog::~ShareAudioDialog()
{
   // Clean up the temp file when the dialog is closed
   if (!mFilePath.empty() && wxFileExists(mFilePath))
      wxRemoveFile(mFilePath);
}

void ShareAudioDialog::Populate(ShuttleGui& s)
{
   mInitialStatePanel.PopulateInitialStatePanel(s);
   mProgressPanel.PopulateProgressPanel(s);

   s.StartHorizontalLay(wxEXPAND, 0);
   {
      s.StartInvisiblePanel(16);
      {
         s.SetBorder(0);
         s.StartHorizontalLay(wxEXPAND, 0);
         {
            s.AddSpace(0, 0, 1);

            mCancelButton = s.AddButton(XXO("&Cancel"));
            mCancelButton->Bind(wxEVT_BUTTON, [this](auto) { OnCancel(); });

            mContinueButton = s.AddButton(XXO("C&ontinue"));
            mContinueButton->Bind(wxEVT_BUTTON, [this](auto) { OnContinue(); });

            mGotoButton = s.AddButton(XXO("&Go to my file"));

            mCloseButton = s.AddButton(XXO("C&lose"));
            mCloseButton->Bind(wxEVT_BUTTON, [this](auto) { OnClose(); });
         }
         s.EndHorizontalLay();
      }
      s.EndInvisiblePanel();
   }
   s.EndHorizontalLay();

   // This two buttons are only used in the end of
   // authorised upload flow
   mGotoButton->Hide();
   mCloseButton->Hide();
}

void ShareAudioDialog::OnCancel()
{
   // If export has started, notify it that it should be canceled
   if (mExportProgressHelper != nullptr)
      static_cast<ExportProgressHelper&>(*mExportProgressHelper).Cancel();
   // If upload is running - ask it to discard the result
   if (mServices->uploadPromise)
      mServices->uploadPromise->DiscardResult();

   EndModal(wxID_CANCEL);
}

void ShareAudioDialog::OnContinue()
{
   mContinueAction();
}

void ShareAudioDialog::OnClose()
{
   EndModal(wxID_CLOSE);
}


wxString ShareAudioDialog::ExportProject()
{
   mExportProgressHelper = std::make_unique<ExportProgressHelper>(*this);

   Exporter e { mProject };

   auto helper = GetUploadHelper(e);

   wxString path;

   if (!helper || !DoExport(mProject, e, *helper, mExportProgressHelper, path))
      return {};

   return path;
}

void ShareAudioDialog::StartUploadProcess()
{
   mInitialStatePanel.root->Hide();
   mProgressPanel.root->Show();

   mProgressPanel.linkPanel->Hide();
   mProgressPanel.info->Hide();

   mContinueButton->Hide();

   Layout();
   Fit();

   ResetProgress();

   mFilePath = ExportProject();

   if (mFilePath.empty())
   {
      if (!static_cast<ExportProgressHelper&>(*mExportProgressHelper)
              .WasCancelled())
      {
         HandleExportFailure();
      }

      return;
   }

   mProgressPanel.title->SetLabel(XO("Uploading audio...").Translation());
   ResetProgress();

   mServices->uploadPromise = mServices->uploadService.Upload(
      mFilePath,
      mProject.GetProjectName(),
      [this](const auto& result)
      {
         CallAfter(
            [this, result]()
            {
               if (result.result == UploadOperationCompleted::Result::Success)
                  HandleUploadSucceeded(result.finishUploadURL, result.audioSlug);
               else if (result.result != UploadOperationCompleted::Result::Aborted)
                  HandleUploadFailed(result.errorMessage);
            });
      },
      [this](auto current, auto total)
      {
         CallAfter(
            [this, current, total]()
            {
               UpdateProgress(current, total);
            });
      });
}

void ShareAudioDialog::HandleUploadSucceeded(
   std::string_view finishUploadURL, std::string_view audioSlug)
{
   mProgressPanel.timePanel->Hide();
   mProgressPanel.title->SetLabel(XO("Upload complete!").Translation());
   mProgressPanel.info->Show();

   if (!GetOAuthService().HasAccessToken())
   {
      mProgressPanel.info->SetLabel(
         "By pressing continue, you will be taken to audio.com and given a sharable link.");
      mProgressPanel.info->Wrap(mProgressPanel.info->GetSize().GetWidth());

      mContinueAction = [this, url = std::string(finishUploadURL)]()
      {
         EndModal(wxID_CLOSE);
         OpenInDefaultBrowser({ url });
      };

      mContinueButton->Show();
   }
   else
   {
      auto sharableLink = wxString::Format(
         "https://audio.com/%s/%s", GetUserService().GetUserSlug(),
         audacity::ToWXString(audioSlug));

      mGotoButton->Show();
      mCloseButton->Show();
      mCancelButton->Hide();

      mGotoButton->Bind(
         wxEVT_BUTTON,
         [this, url = sharableLink](auto)
         {
            EndModal(wxID_CLOSE);
            OpenInDefaultBrowser({ url });
         });

      mProgressPanel.link->SetValue(sharableLink);
      mProgressPanel.linkPanel->Show();
   }

   Layout();
   Fit();
}

void ShareAudioDialog::HandleUploadFailed(std::string_view errorMessage)
{
   EndModal(wxID_ABORT);

   BasicUI::ShowErrorDialog(
      {}, XO("Upload error"),
      XO("We are unable to upload this file. Please try again and make sure to link to your audio.com account before uploading."),
      {},
      BasicUI::ErrorDialogOptions { BasicUI::ErrorDialogType::ModalError }.Log(
         audacity::ToWString(errorMessage)));
}

void ShareAudioDialog::HandleExportFailure()
{
   EndModal(wxID_ABORT);

   BasicUI::ShowErrorDialog(
      {}, XO("Export error"),
      XO("We are unable to prepare this file for uploading."), {},
      BasicUI::ErrorDialogOptions { BasicUI::ErrorDialogType::ModalError });
}

void ShareAudioDialog::ResetProgress()
{
   mStageStartTime = Clock::now();
   mLastUIUpdateTime = mStageStartTime;

   mProgressPanel.elapsedTime->SetLabel(" 00:00:00");
   mProgressPanel.remainingTime->SetLabel(" 00:00:00");
   mProgressPanel.progress->SetValue(0);

   mLastProgressValue = 0;

   BasicUI::Yield();
}

namespace
{
void SetTimeLabel(wxStaticText* label, std::chrono::milliseconds time)
{
   wxTimeSpan tsElapsed(0, 0, 0, time.count());

   label->SetLabel(tsElapsed.Format(wxT(" %H:%M:%S")));
   label->SetName(label->GetLabel());
   label->Update();
}
}

void ShareAudioDialog::UpdateProgress(uint64_t current, uint64_t total)
{
   const auto now = Clock::now();

   if (current == 0)
      return;

   if (current > total)
      current = total;

   if (mLastProgressValue != current)
   {
      constexpr int scale = 10000;

      mLastProgressValue = static_cast<int>(current);

      mProgressPanel.progress->SetRange(scale);
      mProgressPanel.progress->SetValue((current * scale) / total);

      if (current == total && mServices->uploadPromise)
      {
         mProgressPanel.timePanel->Hide();
         mProgressPanel.title->SetLabel(XO("Finalizing upload...").Translation());
      }
   }

   const auto elapsedSinceUIUpdate = now - mLastUIUpdateTime;

   constexpr auto uiUpdateTimeout = std::chrono::milliseconds(500);

   if (elapsedSinceUIUpdate < uiUpdateTimeout && current < total)
      return;

   mLastUIUpdateTime = now;

   const auto elapsed = now - mStageStartTime;

   SetTimeLabel(
      mProgressPanel.elapsedTime,
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed));

   const auto estimate = elapsed * total / current;
   const auto remains = (mStageStartTime + estimate) - now;

   SetTimeLabel(
      mProgressPanel.remainingTime,
      std::chrono::duration_cast<std::chrono::milliseconds>(remains));
}

ShareAudioDialog::InitialStatePanel::InitialStatePanel()
    : mUserDataChangedSubscription(
         GetUserService().Subscribe([this](const auto&) { UpdateUserData(); }))
{
}

void ShareAudioDialog::InitialStatePanel::PopulateInitialStatePanel(
   ShuttleGui& s)
{
   root = s.StartInvisiblePanel();
   s.StartVerticalLay(wxEXPAND, 1);
   {
      s.SetBorder(16);

      s.StartHorizontalLay(wxEXPAND, 0);
      {
         avatar = safenew UserImage(s.GetParent(), avatarSize);

         s.AddWindow(avatar);

         s.StartVerticalLay(wxEXPAND, 1);
         {
            s.SetBorder(0);
            s.AddSpace(0, 0, 1);
            name = s.AddVariableText(XO("Anonymous"));
            s.AddSpace(0, 0, 1);
         }
         s.EndVerticalLay();

         s.AddSpace(0, 0, 1);

         s.StartVerticalLay(wxEXPAND, 1);
         {
            s.AddSpace(0, 0, 1);

            s.SetBorder(16);
            oauthButton = s.AddButton(XXO("&Link Account"));
            oauthButton->Bind(
               wxEVT_BUTTON, [this](auto) { OnLinkButtonPressed(); });
            s.AddSpace(0, 0, 1);
         }
         s.EndVerticalLay();
      }
      s.EndHorizontalLay();

      s.SetBorder(0);

      s.AddWindow(safenew wxStaticLine { s.GetParent() }, wxEXPAND);

      if (!wasOpened.Read())
         PopulateFirstTimeNotice(s);
      else
      {
         s.AddSpace(16);
         s.StartHorizontalLay(wxEXPAND, 0);
         {
            s.AddSpace(30, 0, 0);
            s.AddFixedText(XO("Press \"Continue\" to upload to audio.com"));
         }
         s.EndHorizontalLay();
      }

   }
   s.EndVerticalLay();
   s.EndInvisiblePanel();

   UpdateUserData();
}

void ShareAudioDialog::InitialStatePanel::PopulateFirstTimeNotice(ShuttleGui& s)
{
   s.AddSpace(16);
   s.StartInvisiblePanel();
   s.SetBorder(30);
   {
      AccessibleLinksFormatter privacyPolicy(XO(
         "Your audio will be uploaded to our sharing service: %s,%%which requires a free account to use.\n\nIf you have problems uploading, try the Link Account button."));

      privacyPolicy.FormatLink(
         L"%s", XO("audio.com"),
         "https://audio.com");

      privacyPolicy.FormatLink(
         L"%%", TranslatableString {},
         AccessibleLinksFormatter::LinkClickedHandler {});

      privacyPolicy.Populate(s);
   }
   s.EndInvisiblePanel();

   wasOpened.Write(true);
   gPrefs->Flush();
}

void ShareAudioDialog::InitialStatePanel::UpdateUserData()
{
   auto layoutUpdater = finally(
      [parent = root->GetParent(), this]()
      {
         oauthButton->Fit();
         parent->Layout();
      });

   auto& oauthService = GetOAuthService();

   if (!oauthService.HasRefreshToken())
   {
      name->SetLabel(XO("Anonymous").Translation());
      avatar->SetBitmap(theTheme.Bitmap(bmpAnonymousUser));
      oauthButton->SetLabel(XXO("&Link Account").Translation());

      return;
   }

   if (!oauthService.HasAccessToken())
      oauthService.ValidateAuth({});

   oauthButton->SetLabel(XXO("&Unlink Account").Translation());

   auto& userService = GetUserService();

   const auto displayName = userService.GetDisplayName();

   if (!displayName.empty())
      name->SetLabel(displayName);

   const auto avatarPath = userService.GetAvatarPath();

   if (!avatarPath.empty())
      avatar->SetBitmap(avatarPath);
}

void ShareAudioDialog::InitialStatePanel::OnLinkButtonPressed()
{
   auto& oauthService = GetOAuthService();

   if (oauthService.HasAccessToken())
      oauthService.UnlinkAccount();
   else
   {
      OpenInDefaultBrowser(
         { audacity::ToWXString(GetServiceConfig().GetOAuthLoginPage()) });

#ifdef HAS_CUSTOM_URL_HANDLING
      if (!URLSchemesRegistry::Get().IsURLHandlingSupported())
#endif
      {
         LinkAccountDialog dlg(root);
         dlg.ShowModal();
      }
   }
}

void ShareAudioDialog::ProgressPanel::PopulateProgressPanel(ShuttleGui& s)
{
   root = s.StartInvisiblePanel(16);
   root->Hide();
   s.StartVerticalLay(wxEXPAND, 1);
   {
      s.SetBorder(0);

      title = s.AddVariableText(XO("Preparing audio..."));
      s.AddSpace(0, 16, 0);

      progress = safenew wxGauge { s.GetParent(), wxID_ANY, 100 };
      s.AddWindow(progress, wxEXPAND);

      timePanel = s.StartInvisiblePanel();
      {
         s.AddSpace(0, 16, 0);

         s.StartWrapLay();
         {
            s.AddFixedText(XO("Elapsed Time:"));
            elapsedTime = s.AddVariableText(Verbatim(" 00:00:00"));
         }
         s.EndWrapLay();

         s.StartWrapLay();
         {
            s.AddFixedText(XO("Remaining Time:"));
            remainingTime = s.AddVariableText(Verbatim(" 00:00:00"));
         }
         s.EndWrapLay();
      }
      s.EndInvisiblePanel();

      linkPanel = s.StartInvisiblePanel();
      {
         s.AddSpace(0, 16, 0);

         s.AddFixedText(XO("Sharable link"));

         s.StartHorizontalLay(wxEXPAND, 0);
         {
            link = s.AddTextBox(TranslatableString {}, "https://audio.com", 60);
            link->SetName(XO("Sharable link").Translation());
            link->SetEditable(false);

            copyButton = s.AddButton(XO("Copy"));
            copyButton->Bind(
               wxEVT_BUTTON,
               [this](auto)
               {
                  if (wxTheClipboard->Open())
                  {
                     wxTheClipboard->SetData(
                        safenew wxTextDataObject(link->GetValue()));
                     wxTheClipboard->Close();
                  }
               });
         }
         s.EndHorizontalLay();
      }
      s.EndInvisiblePanel();

      s.AddSpace(0, 16, 0);
      info = s.AddVariableText(XO("Only people you share this link with can access your audio"));
   }
   s.EndVerticalLay();
   s.EndInvisiblePanel();

   wxFont font = elapsedTime->GetFont();
   font.MakeBold();

   elapsedTime->SetFont(font);
   remainingTime->SetFont(font);
}
} // namespace cloud::audiocom
