/**********************************************************************

  Audacity: A Digital Audio Editor

  @file VST3EffectsModule.h

  @author Vitaly Sverchinsky

  @brief Part of Audacity VST3 module

**********************************************************************/

#pragma once

#include <unordered_map>
#include <memory>

#include "ModuleInterface.h"

namespace VST3
{
   namespace Hosting
   {
      class Module;
   }
}

/**
 * \brief VST3Effect factory.
 */
class VST3EffectsModule final : public ModuleInterface
{
   //Holds weak pointers to the unique modules which were accessed
   //through VST3EffectsModule::GetModule() during the lifetime.
   std::unordered_map<wxString, std::weak_ptr<VST3::Hosting::Module>> mModules;

   //Attempts to look up for a module, or load it from the hard drive if
   //none was found (or not valid pointers)
   std::shared_ptr<VST3::Hosting::Module> GetModule(const wxString& path);

public:

   PluginPath GetPath() override;
   ComponentInterfaceSymbol GetSymbol() override;
   VendorSymbol GetVendor() override;
   wxString GetVersion() override;
   TranslatableString GetDescription() override;

   bool Initialize() override;
   void Terminate() override;
   EffectFamilySymbol GetOptionalFamilySymbol() override;
   const FileExtensions& GetFileExtensions() override;
   FilePath InstallPath() override;
   bool AutoRegisterPlugins(PluginManagerInterface& pluginManager) override;
   PluginPaths FindPluginPaths(PluginManagerInterface& pluginManager) override;
   unsigned DiscoverPluginsAtPath(const PluginPath& path, TranslatableString& errMsg,
      const RegistrationCallback& callback) override;
   bool IsPluginValid(const PluginPath& path, bool bFast) override;
   std::unique_ptr<ComponentInterface> CreateInstance(const PluginPath& path) override;

};
