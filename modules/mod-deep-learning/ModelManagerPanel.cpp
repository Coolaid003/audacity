/**********************************************************************

   Audacity: A Digital Audio Editor
   Audacity(R) is copyright (c) 1999-2021 Audacity Team.

   ModelManagerPanel.cpp
   Hugo Flores Garcia

******************************************************************/

#include "ModelManagerPanel.h"

#include "ActiveModel.h"
#include "CodeConversions.h"
#include "ExploreHuggingFaceDialog.h"
#include "effects/Effect.h"

#include "Shuttle.h"
#include "ShuttleGui.h"

#include <wx/display.h>
#include <wx/scrolwin.h>
#include <wx/range.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/textdlg.h>
#include <wx/stattext.h>
#include <wx/hyperlink.h>

#include "widgets/AudacityTextEntryDialog.h"
#include "Internat.h"
#include "AllThemeResources.h"
#include "Theme.h"

// ModelManagerPanel
ModelManagerPanel::ModelManagerPanel(
   wxWindow *parent, Effect *effect,
   std::shared_ptr<ActiveModel> pActiveModel, const std::string &deepEffectID)
   : wxPanelWrapper(parent)
   , mEffect{ effect }
   , mActiveModel(move(pActiveModel))
   , mDeepEffectID{ deepEffectID }
{
   mSubscription =
      mActiveModel->Subscribe( *this, &ModelManagerPanel::SetSelectedCard );

   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);
   Layout();
   Fit();
   Center();
}

void ModelManagerPanel::PopulateOrExchange(ShuttleGui & S)
{
   DeepModelManager &manager = DeepModelManager::Get();
   S.StartVerticalLay(true);
   {
      mTools = safenew ManagerToolsPanel(S.GetParent(), this);
      // mTools->PopulateOrExchange(S);
      S.AddWindow(mTools);

      S.StartMultiColumn(2, wxEXPAND);
      {
         // this scroller is populated dynamically, 
         // as cards are fetched by the model manager
         mScroller = S.StartScroller(wxVSCROLL);
         {
         }
         S.EndScroller();

         wxSize size(cardPanel_w+50, detailedCardPanel_h);
         wxSize vsize(cardPanel_w+cardPanel_x_offset, 
                     detailedCardPanel_h);
         mScroller->SetVirtualSize(vsize);
         mScroller->SetSize(size); 
         mScroller->SetMinSize(size); 
         // mScroller->SetMaxSize(size);
         mScroller->SetWindowStyle(wxBORDER_SIMPLE);
         mScroller->SetScrollRate(0, 10);

         // this panel changes contents according to the card the
         // user has selected. 
         mDetailedPanel = safenew DetailedModelCardPanel(
               S.GetParent(), wxID_ANY,
               manager.GetEmptyCard(), mEffect, mActiveModel);

         S.AddWindow(mDetailedPanel, wxALIGN_TOP);
      }
      S.EndMultiColumn();
   }
   S.EndVerticalLay();

   FetchCards();
}

void ModelManagerPanel::AddCard(ModelCardHolder card)
{
   DeepModelManager &manager = DeepModelManager::Get();

   mScroller->EnableScrolling(true, true);
   std::string repoId = card->GetRepoID();
   mPanels[repoId] =
      std::make_unique<SimpleModelCardPanel>(mScroller, wxID_ANY,
         card, mEffect, mActiveModel);

   // if this is the first card we're adding, go ahead and select it
   if (mPanels.size() == 1)
   {
      mActiveModel->SetModel(*mEffect, card);
   }

   ShuttleGui S(mScroller, eIsCreating);
   S.AddWindow(mPanels[repoId].get(), wxEXPAND);
   // mPanels[repoId]->PopulateOrExchange(S);

   wxSizer *sizer = mScroller->GetSizer();
   if (sizer)
   {
      sizer->SetSizeHints(mScroller);
   }
   mScroller->FitInside();
   mScroller->Layout();
   mScroller->GetParent()->Layout();
}

CardFetchedCallback ModelManagerPanel::GetCardFetchedCallback()
{
   CardFetchedCallback onCardFetched = [this](bool success, ModelCardHolder card)
   {
      this->CallAfter(
         [this, success, card]()
         {
            if (success)
            {
               bool found = mPanels.find(card->GetRepoID()) != mPanels.end();
               bool effectTypeMatches = card->effect_type() == mDeepEffectID;
               if (!found && effectTypeMatches)
                  this->AddCard(card);
            }
         }
      );
   };

   return onCardFetched;
}

void ModelManagerPanel::FetchCards()
{
   DeepModelManager &manager = DeepModelManager::Get();
   CardFetchedCallback onCardFetched = GetCardFetchedCallback();

   CardFetchProgressCallback onCardFetchedProgress = [this](int64_t current, int64_t total)
   {
      this->CallAfter(
         [this, current, total]()
         {
            if (mTools)
               this->mTools->SetFetchProgress(current, total);
         }
      );
   };

   manager.FetchModelCards(onCardFetched, onCardFetchedProgress);
   manager.FetchLocalCards(onCardFetched);
}

void ModelManagerPanel::SetSelectedCard(ModelCardHolder card)
{
   wxSize oldSize = GetSize();
   wxSize oldScrollerSize = mScroller->GetSize();
   int oldPos = mScroller->GetScrollPos(wxVERTICAL);
   // set all other card panels to disabled
   for (auto& pair : mPanels)
   {
      pair.second->SetModelStatus(ModelCardPanel::ModelStatus::Disabled);

      if (card)
      {
         if (pair.first == card->GetRepoID())
            pair.second->SetModelStatus(ModelCardPanel::ModelStatus::Enabled);
      }
   }

   // configure the detailed panel
   if (card)
   {
      mDetailedPanel->PopulateWithNewCard(card);
      mDetailedPanel->SetModelStatus(ModelCardPanel::ModelStatus::Enabled);
   }

   SetSize(oldSize);
   mScroller->SetSize(oldScrollerSize);
   mScroller->Refresh();
   mScroller->Scroll(0, oldPos);
   // mScroller->FitInside();
   // mScroller->Layout();
   // mScroller->GetParent()->Layout();
}

// ManagerToolsPanel

ManagerToolsPanel::ManagerToolsPanel(wxWindow *parent, ModelManagerPanel *panel)
   : wxPanelWrapper((wxWindow *)parent, 
                     wxID_ANY, 
                     wxDefaultPosition, 
                     getManagerToolsPanelSize()), 
       mManagerPanel{ panel }
{
   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);

   SetWindowStyle(wxBORDER_SIMPLE);
   // Fit();
   Layout();
   // Center();
   // SetMinSize(GetSize());
   Refresh();
}

void ManagerToolsPanel::PopulateOrExchange(ShuttleGui &S)
{
   S.StartHorizontalLay(wxLEFT | wxEXPAND, true);
   {
      mAddRepoButton = S.AddButton(XO("Add From HuggingFace"));
      mExploreButton = S.AddButton(XO("Explore Models"));
      mFetchStatus = S.AddVariableText(XO("Fetching models..."), 
                                 true, wxALIGN_CENTER_VERTICAL);
   }
   S.EndHorizontalLay();

   mAddRepoButton->Bind(wxEVT_BUTTON, &ManagerToolsPanel::OnAddRepo, this);
   mExploreButton->Bind(wxEVT_BUTTON, &ManagerToolsPanel::OnExplore, this);
}

void ManagerToolsPanel::OnAddRepo(wxCommandEvent & WXUNUSED(event))
{
   DeepModelManager &manager = DeepModelManager::Get();

   TranslatableString msg = XO("Enter a HuggingFace Repo ID \n"
                    "For example: \"huggof/ConvTasNet-DAMP-Vocals\"\n");
   TranslatableString caption = XO("AddRepo");
   AudacityTextEntryDialog dialog(this, msg, caption, wxEmptyString, wxOK | wxCANCEL );

   if (dialog.ShowModal() == wxID_OK)
   {
      wxString repoId = dialog.GetValue();
      // wrap the card fetched callback 
      CardFetchedCallback onCardFetched(
         [this, repoId](bool success, ModelCardHolder card)
         {
            this->CallAfter(
               [this, success, card, repoId]()
               {
                  this->mManagerPanel->GetCardFetchedCallback()(success, card);
                  if (!success)
                  {
                     mManagerPanel->mEffect->MessageBox(
                        XO("An error occurred while fetching  %s from HuggingFace. "
                        "This model may be broken. If you are the model developer, "
                         "check the error log for more details.")
                           .Format(repoId)
                     );
                  }
               }
            );
         }
      );

      manager.FetchCard(audacity::ToUTF8(repoId), onCardFetched);
   }
}

void ManagerToolsPanel::SetFetchProgress(int64_t current, int64_t total)
{
   if (!mFetchStatus)
      return;

   if (total == 0)
   {
      wxString translated = XO("Error fetching models.").Translation();
      mFetchStatus->SetLabel(translated);
   }

   if (current < total)
   {
      wxString translated = XO("Fetching %d out of %d")
            .Format((int)current, (int)total).Translation();
      mFetchStatus->SetLabel(translated);
   }

   if (current == total)
   {
      mFetchStatus->SetLabel(XO("Manager ready.").Translation());
   }
}

void ManagerToolsPanel::OnExplore(wxCommandEvent & WXUNUSED(event))
{
   ExploreHuggingFaceDialog dialog(mManagerPanel->GetParent());
   dialog.ShowModal();
}

int getScreenWidth(float scale) 
{
   wxDisplay display((int)0);
   wxRect screen = display.GetClientArea();
   return static_cast<int>(screen.GetWidth() * scale);
}

int getScreenHeight(float scale)
{
   wxDisplay display((int)0);
   wxRect screen = display.GetClientArea();
   return static_cast<int>(screen.GetHeight() * scale);
}

wxSize getManagerToolsPanelSize()
{
   int screenHeight = getScreenHeight();
   float scale = screenHeight < 1000 ? .06 : .06 - static_cast<float>(screenHeight)/100000;
   return wxSize(managerPanel_w, getScreenHeight(scale));
}