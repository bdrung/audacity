/**********************************************************************

  Audacity: A Digital Audio Editor

  ModuleManager.cpp

  Dominic Mazzoni
  James Crook


*******************************************************************//*!

\file ModuleManager.cpp
\brief Based on LoadLadspa, this code loads pluggable Audacity 
extension modules.  It also has the code to (a) invoke a script
server and (b) invoke a function returning a replacement window,
i.e. an alternative to the usual interface, for Audacity.

*//*******************************************************************/

#include <wx/dynlib.h>
#include <wx/list.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/string.h>
#include <wx/filename.h>

#include "Audacity.h"
#include "AudacityApp.h"
#include "FileNames.h"
#include "Internat.h"
#include "PluginManager.h"

#include "commands/ScriptCommandRelay.h"
#include <NonGuiThread.h>  // header from libwidgetextra

#include "audacity/PluginInterface.h"

#ifdef EXPERIMENTAL_MODULE_PREFS
#include "Prefs.h"
#include "./prefs/ModulePrefs.h"
#endif

#include "ModuleManager.h"
#include "widgets/MultiDialog.h"

#define initFnName      "ExtensionModuleInit"
#define versionFnName   "GetVersionString"
#define scriptFnName    "RegScriptServerFunc"
#define mainPanelFnName "MainPanelFunc"

typedef wxWindow * pwxWindow;
typedef int (*tModuleInit)(int);
//typedef wxString (*tVersionFn)();
typedef wxChar * (*tVersionFn)();
typedef pwxWindow (*tPanelFn)(int);

// This variable will hold the address of a subroutine in 
// a DLL that can hijack the normal panel.
static tPanelFn pPanelHijack=NULL;

// Next two commented out lines are handy when investigating
// strange DLL behaviour.  Instead of dynamic linking,
// link the library which has the replacement panel statically.
// Give the address of the routine here.
// This is a great help in identifying missing 
// symbols which otherwise cause a dll to unload after loading
// without an explanation as to why!
//extern wxWindow * MainPanelFunc( int i );
//tPanelFn pPanelHijack=&MainPanelFunc;

/// IF pPanelHijack has been found in a module DLL
/// THEN when this function is called we'll go and
/// create that window instead of the normal one.
wxWindow * MakeHijackPanel()
{
   if( pPanelHijack == NULL )
      return NULL;
   return pPanelHijack(0);
}

// This variable will hold the address of a subroutine in a DLL that
// starts a thread and reads script commands.
static tpRegScriptServerFunc scriptFn;

Module::Module(const wxString & name)
{
   mName = name;
   mLib = new wxDynamicLibrary();
   mDispatch = NULL;
}

Module::~Module()
{
   delete mLib;
}

bool Module::Load()
{
   if (mLib->IsLoaded()) {
      if (mDispatch) {
         return true;
      }
      return false;
   }

   if (!mLib->Load(mName, wxDL_LAZY)) {
      return false;
   }

   // Check version string matches.  (For now, they must match exactly)
   tVersionFn versionFn = (tVersionFn)(mLib->GetSymbol(wxT(versionFnName)));
   if (versionFn == NULL){
      wxString ShortName = wxFileName( mName ).GetName();
      wxMessageBox(wxString::Format(_("The module %s does not provide a version string.\nIt will not be loaded."), ShortName.c_str()), _("Module Unsuitable"));
      wxLogMessage(wxString::Format(_("The module %s does not provide a version string.  It will not be loaded."), mName.c_str()));
      mLib->Unload();
      return false;
   }

   wxString moduleVersion = versionFn();
   if( !moduleVersion.IsSameAs(AUDACITY_VERSION_STRING)) {
      wxString ShortName = wxFileName( mName ).GetName();
      wxMessageBox(wxString::Format(_("The module %s is matched with Audacity version %s.\n\nIt will not be loaded."), ShortName.c_str(), moduleVersion.c_str()), _("Module Unsuitable"));
      wxLogMessage(wxString::Format(_("The module %s is matched with Audacity version %s.  It will not be loaded."), mName.c_str(), moduleVersion.c_str()));
      mLib->Unload();
      return false;
   }

   mDispatch = (fnModuleDispatch) mLib->GetSymbol(wxT(ModuleDispatchName));
   if (!mDispatch) {
      // Module does not provide a dispatch function...
      // That can be OK, as long as we never try to call it.
      return true;
   }

   // However if we do have it and it does not work, 
   // then the module is bad.
   bool res = ((mDispatch(ModuleInitialize))!=0);
   if (res) {
      return true;
   }

   mDispatch = NULL;
   return false;
}

void Module::Unload()
{
   if (mLib->IsLoaded()) {
      mDispatch(ModuleTerminate);
   }

   mLib->Unload();
}

int Module::Dispatch(ModuleDispatchTypes type)
{
   if (mLib->IsLoaded())
      if( mDispatch != NULL )
         return mDispatch(type);

   return 0;
}

void * Module::GetSymbol(wxString name)
{
   return mLib->GetSymbol(name);
}

// ============================================================================
//
// ModuleManager
//
// ============================================================================

// The one and only ModuleManager
ModuleManager ModuleManager::mInstance;

// Provide builtin modules a means to identify themselves
static wxArrayPtrVoid *pBuiltinModuleList = NULL;
void RegisterBuiltinModule(ModuleMain moduleMain)
{
   if (pBuiltinModuleList == NULL)
      pBuiltinModuleList = new wxArrayPtrVoid;

   pBuiltinModuleList->Add((void *)moduleMain);
   return;
}

// ----------------------------------------------------------------------------
// Creation/Destruction
// ----------------------------------------------------------------------------

ModuleManager::ModuleManager()
{
}

ModuleManager::~ModuleManager()
{
   size_t cnt = mModules.GetCount();

   for (size_t ndx = 0; ndx < cnt; ndx++) {
      delete (Module *) mModules[ndx];
   }
   mModules.Clear();

   for (ModuleMap::iterator iter = mDynModules.begin(); iter != mDynModules.end(); iter++)
   {
      ModuleInterface *mod = iter->second;
      delete mod;
   }
   mDynModules.clear();
   if( pBuiltinModuleList != NULL )
      delete pBuiltinModuleList;
}

// static 
void ModuleManager::Initialize(CommandHandler &cmdHandler)
{
   wxArrayString audacityPathList = wxGetApp().audacityPathList;
   wxArrayString pathList;
   wxArrayString files;
   wxString pathVar;
   size_t i;

   // Code from LoadLadspa that might be useful in load modules.
   pathVar = wxGetenv(wxT("AUDACITY_MODULES_PATH"));
   if (pathVar != wxT(""))
      wxGetApp().AddMultiPathsToPathList(pathVar, pathList);

   for (i = 0; i < audacityPathList.GetCount(); i++) {
      wxString prefix = audacityPathList[i] + wxFILE_SEP_PATH;
      wxGetApp().AddUniquePathToPathList(prefix + wxT("modules"),
                                         pathList);
   }

   #if defined(__WXMSW__)
   wxGetApp().FindFilesInPathList(wxT("*.dll"), pathList, files);
   #else
   wxGetApp().FindFilesInPathList(wxT("*.so"), pathList, files);
   #endif

   for (i = 0; i < files.GetCount(); i++) {
      // As a courtesy to some modules that might be bridges to
      // open other modules, we set the current working
      // directory to be the module's directory.
      wxString saveOldCWD = ::wxGetCwd();
      wxString prefix = ::wxPathOnly(files[i]);
      ::wxSetWorkingDirectory(prefix);

#ifdef EXPERIMENTAL_MODULE_PREFS
      int iModuleStatus = ModulePrefs::GetModuleStatus( files[i] );
      if( iModuleStatus == kModuleDisabled )
         continue;
      if( iModuleStatus == kModuleFailed )
         continue;
      // New module?  You have to go and explicitly enable it.
      if( iModuleStatus == kModuleNew ){
         // To ensure it is noted in config file and so
         // appears on modules page.
         ModulePrefs::SetModuleStatus( files[i], kModuleNew);
         continue;
      }

      if( iModuleStatus == kModuleAsk )
#endif
      // JKC: I don't like prompting for the plug-ins individually
      // I think it would be better to show the module prefs page,
      // and let the user decide for each one.
      {
         wxString ShortName = wxFileName( files[i] ).GetName();
         wxString msg;
         msg.Printf(_("Module \"%s\" found."), ShortName.c_str());
         msg += _("\n\nOnly use modules from trusted sources");
         const wxChar *buttons[] = {_("Yes"), _("No"), NULL};  // could add a button here for 'yes and remember that', and put it into the cfg file.  Needs more thought.
         int action;
         action = ShowMultiDialog(msg, _("Audacity Module Loader"), buttons, _("Try and load this module?"), false);
#ifdef EXPERIMENTAL_MODULE_PREFS
         // If we're not prompting always, accept the answer permanantly
         if( iModuleStatus == kModuleNew ){
            iModuleStatus = (action==1)?kModuleDisabled : kModuleEnabled;
            ModulePrefs::SetModuleStatus( files[i], iModuleStatus );
         }
#endif
         if(action == 1){   // "No"
            continue;
         }
      }
#ifdef EXPERIMENTAL_MODULE_PREFS
      // Before attempting to load, we set the state to bad.
      // That way, if we crash, we won't try again.
      ModulePrefs::SetModuleStatus( files[i], kModuleFailed );
#endif

      Module *module = new Module(files[i]);
      if (module->Load())   // it will get rejected if there  are version problems
      {
         Get().mModules.Add(module);
         // We've loaded and initialised OK.
         // So look for special case functions:
         wxLogNull logNo; // Don't show wxWidgets errors if we can't do these. (Was: Fix bug 544.)
         // (a) for scripting.
         if( scriptFn == NULL )
            scriptFn = (tpRegScriptServerFunc)(module->GetSymbol(wxT(scriptFnName)));
         // (b) for hijacking the entire Audacity panel.
         if( pPanelHijack==NULL )
         {
            pPanelHijack = (tPanelFn)(module->GetSymbol(wxT(mainPanelFnName)));
         }
#ifdef EXPERIMENTAL_MODULE_PREFS
         // Loaded successfully, restore the status.
         ModulePrefs::SetModuleStatus( files[i], iModuleStatus);
#endif
      }
      else {
         // No need to save status, as we already set kModuleFailed.
         delete module;
      }
      ::wxSetWorkingDirectory(saveOldCWD);
   }
   // After loading all the modules, we may have a registered scripting function.
   if(scriptFn)
   {
      ScriptCommandRelay::SetCommandHandler(cmdHandler);
      ScriptCommandRelay::SetRegScriptServerFunc(scriptFn);
      NonGuiThread::StartChild(&ScriptCommandRelay::Run);
   }
}

// static
int ModuleManager::Dispatch(ModuleDispatchTypes type)
{
   size_t cnt = Get().mModules.GetCount();

   for (size_t ndx = 0; ndx < cnt; ndx++) {
      Module *module = (Module *)Get().mModules[ndx];

      module->Dispatch(type);
   }
   return 0;
}

// ============================================================================
//
// Return reference to singleton
//
// (Thread-safe...no active threading during construction or after destruction)
// ============================================================================
ModuleManager & ModuleManager::Get()
{
   return mInstance;
}

void ModuleManager::InitializeBuiltins()
{
   PluginManager & pm = PluginManager::Get();

   if (pBuiltinModuleList==NULL)
      return;

   for (size_t i = 0, cnt = pBuiltinModuleList->GetCount(); i < cnt; i++)
   {
      ModuleMain audacityMain = (ModuleMain) (*pBuiltinModuleList)[i];
      ModuleInterface *module = audacityMain(this, NULL);

      mDynModules[module->GetID()] = module;

      module->Initialize();

      // First, we need to remember it 
      pm.RegisterModulePlugin(module);

      // Now, allow the module to auto-register children
      module->AutoRegisterPlugins(pm);
   }
}

// static 
void ModuleManager::EarlyInit()
{
   InitializeBuiltins();
}

bool ModuleManager::DiscoverProviders(wxArrayString & providers)
{
   wxArrayString provList;
   wxArrayString pathList;

   // Code from LoadLadspa that might be useful in load modules.
   wxString pathVar = wxString::FromUTF8(getenv("AUDACITY_MODULES_PATH"));

   if (pathVar != wxT(""))
   {
      wxGetApp().AddMultiPathsToPathList(pathVar, pathList);
   }
   else
   {
      wxGetApp().AddUniquePathToPathList(FileNames::ModulesDir(), pathList);
   }

#if defined(__WXMSW__)
   wxGetApp().FindFilesInPathList(wxT("*.dll"), pathList, provList);
#elif defined(__WXMAC__)
   wxGetApp().FindFilesInPathList(wxT("*.dylib"), pathList, provList);
#else
   wxGetApp().FindFilesInPathList(wxT("*.so"), pathList, provList);
#endif

   for (int i = 0, cnt = provList.GetCount(); i < cnt; i++)
   {
   wxPrintf(wxT("provider %s\n"), provList[i].c_str());
      providers.push_back(provList[i]);
   }

   return true;
}

bool ModuleManager::DiscoverProvider(const wxString & path)
{
   ModuleInterface *module = LoadModule(path);
   if (module)
   {
      PluginManager & pm = PluginManager::Get();

      // First, we need to remember it 
      pm.RegisterModulePlugin(module);

      // Now, allow the module to auto-register children
      module->AutoRegisterPlugins(pm);

//      UnloadModule(module);
   }

   return true;
}

ModuleInterface *ModuleManager::LoadModule(const wxString & path)
{
   wxDynamicLibrary *lib = new wxDynamicLibrary();

   if (lib->Load(path, wxDL_NOW))
   {
      bool success = false;
      ModuleMain audacityMain = (ModuleMain) lib->GetSymbol(wxSTRINGIZE_T(MODULE_ENTRY),
                                                            &success);
      if (success && audacityMain)
      {
         ModuleInterface *module = audacityMain(this, &path);
         if (module)
         {
            if (module->Initialize())
            {

               mDynModules[module->GetID()] = module;
               mLibs[module] = lib;

               return module;
            }
            module->Terminate();
            delete module;
         }
      }

      lib->Unload();
   }

   delete lib;

   return NULL;
}

void ModuleManager::UnloadModule(ModuleInterface *module)
{
   if (module)
   {
      const PluginID & modID = module->GetID();

      module->Terminate();

      delete module;

      mDynModules.erase(modID);

      if (mLibs.find(module) != mLibs.end())
      {
         mLibs[module]->Unload();
         mLibs.erase(module);
      }
   }
}

void ModuleManager::InitializePlugins()
{
   InitializeBuiltins();

   // Look for dynamic modules here

   for (ModuleMap::iterator iter = mDynModules.begin(); iter != mDynModules.end(); iter++)
   {
      ModuleInterface *mod = iter->second;
      mod->Initialize();
   }
}

void ModuleManager::RegisterModule(ModuleInterface *module)
{
   wxString id = module->GetID();

   if (mDynModules.find(id) != mDynModules.end())
   {
      // TODO:  Should we complain about a duplicate registeration????
      return;
   }

   mDynModules[id] = module;

   PluginManager::Get().RegisterModulePlugin(module);
}

void ModuleManager::FindAllPlugins(PluginIDList & providers, wxArrayString & paths)
{
   PluginManager & pm = PluginManager::Get();

   wxArrayString modIDs;
   wxArrayString modPaths;
   const PluginDescriptor *plug = pm.GetFirstPlugin(PluginTypeModule);
   while (plug)
   {
      modIDs.push_back(plug->GetID());
      modPaths.push_back(plug->GetPath());
      plug = pm.GetNextPlugin(PluginTypeModule);
   }

   for (size_t i = 0, cnt = modIDs.size(); i < cnt; i++)
   {
      PluginID providerID = modIDs[i];

      ModuleInterface *module =
         static_cast<ModuleInterface *>(CreateProviderInstance(providerID, modPaths[i]));
      
      wxArrayString newpaths = module->FindPlugins(pm);
      for (size_t i = 0, cnt = newpaths.size(); i < cnt; i++)
      {
         providers.push_back(providerID);
         paths.push_back(newpaths[i]);
      }
   }
}

wxArrayString ModuleManager::FindPluginsForProvider(const PluginID & providerID,
                                                   const wxString & path)
{
   // Instantiate if it hasn't already been done
   if (mDynModules.find(providerID) == mDynModules.end())
   {
      // If it couldn't be created, just give up and return an empty list
      if (!CreateProviderInstance(providerID, path))
      {
         return wxArrayString();
      }
   }

   return mDynModules[providerID]->FindPlugins(PluginManager::Get());
}

bool ModuleManager::RegisterPlugin(const PluginID & providerID, const wxString & path)
{
   if (mDynModules.find(providerID) == mDynModules.end())
   {
      return false;
   }

   return mDynModules[providerID]->RegisterPlugin(PluginManager::Get(), path);
}

bool ModuleManager::IsProviderBuiltin(const PluginID & providerID)
{
   return mModuleMains.find(providerID) != mModuleMains.end();
}

void *ModuleManager::CreateProviderInstance(const PluginID & providerID,
                                            const wxString & path)
{
   if (path.empty() && mDynModules.find(providerID) != mDynModules.end())
   {
      return mDynModules[providerID];
   }

   return LoadModule(path);
}

void *ModuleManager::CreateInstance(const PluginID & providerID,
                                    const PluginID & ID,
                                    const wxString & path)
{
   if (mDynModules.find(providerID) == mDynModules.end())
   {
      return NULL;
   }

   return mDynModules[providerID]->CreateInstance(ID, path);
}