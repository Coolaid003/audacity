/*!********************************************************************

  Audacity: A Digital Audio Editor

  @file EffectInterface.cpp

**********************************************************************/
#include "EffectInterface.h"
#include <wx/tokenzr.h>

EffectSettingsAccess::~EffectSettingsAccess() = default;

SimpleEffectSettingsAccess::~SimpleEffectSettingsAccess() = default;

const EffectSettings &SimpleEffectSettingsAccess::Get()
{
   return mSettings;
}

void SimpleEffectSettingsAccess::Set(EffectSettings &&settings)
{
   mSettings = std::move(settings);
}

Identifier EffectDefinitionInterface::GetSquashedName(const Identifier &ident)
{
   // Get rid of leading and trailing white space
   auto name = ident.GET();
   name.Trim(true).Trim(false);

   if (name.empty())
      return {};

   wxStringTokenizer st(name, wxT(" "));
   wxString id;

   // CamelCase the name
   while (st.HasMoreTokens()) {
      wxString tok = st.GetNextToken();
      id += tok.Left(1).MakeUpper() + tok.Mid(1).MakeLower();
   }

   return id;
}

EffectDefinitionInterface::~EffectDefinitionInterface() = default;

EffectType EffectDefinitionInterface::GetClassification() const
{
   return GetType();
}

bool EffectDefinitionInterface::EnablesDebug() const
{
   return false;
}

ManualPageID EffectDefinitionInterface::ManualPage() const
{
   return {};
}

FilePath EffectDefinitionInterface::HelpPage() const
{
   return {};
}

bool EffectDefinitionInterface::IsHiddenFromMenus() const
{
   return false;
}

bool EffectDefinitionInterface::VisitSettings(
   SettingsVisitor &, EffectSettings &)
{
   return false;
}

bool EffectDefinitionInterface::VisitSettings(
   ConstSettingsVisitor &, const EffectSettings &) const
{
   return false;
}

auto EffectDefinitionInterfaceEx::MakeSettings() const -> Settings
{
   // Temporary default implementation just saves self
   // Cast away const! Capture pointer to self
   return Settings( const_cast<EffectDefinitionInterfaceEx*>(this) );
}

bool EffectDefinitionInterfaceEx::CopySettingsContents(
   const EffectSettings &src, EffectSettings &dst) const
{
   //! No real copy, just a sanity check on common origin
   return FindMe(src) && FindMe(dst);
}

EffectDefinitionInterfaceEx *
EffectDefinitionInterfaceEx::FindMe(const Settings &settings) const
{
   if (auto ppEffect = settings.cast<EffectDefinitionInterfaceEx*>();
       ppEffect && *ppEffect == this)
      return *ppEffect;
   return nullptr;
}

EffectProcessor::~EffectProcessor() = default;

EffectUIValidator::~EffectUIValidator() = default;

bool EffectUIValidator::UpdateUI()
{
   return true;
}

DefaultEffectUIValidator::DefaultEffectUIValidator(
   EffectUIClientInterface &effect, EffectSettingsAccess &access)
   : mEffect{effect}
   , mAccess{access}
{}

DefaultEffectUIValidator::~DefaultEffectUIValidator()
{
   mEffect.CloseUI();
}

bool DefaultEffectUIValidator::ValidateUI()
{
   bool result {};
   mAccess.ModifySettings([&](EffectSettings &settings){
      result = mEffect.ValidateUI(settings);
   });
   return result;
}

EffectUIClientInterface::~EffectUIClientInterface() = default;

const RegistryPath &CurrentSettingsGroup()
{
   static RegistryPath id{ "CurrentSettings" };
   return id;
}

const RegistryPath &FactoryDefaultsGroup()
{
   static RegistryPath id{ "FactoryDefaults" };
   return id;
}

RegistryPath UserPresetsGroup(const RegistryPath & name)
{
   RegistryPath group = wxT("UserPresets");
   if (!name.empty())
      group += wxCONFIG_PATH_SEPARATOR + name;
   return group;
}
