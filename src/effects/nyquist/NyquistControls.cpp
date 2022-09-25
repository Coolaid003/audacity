/**********************************************************************

  Audacity: A Digital Audio Editor

  @file NyquistControls.cpp

  Dominic Mazzoni

  Paul Licameli split from Nyquist.cpp

******************************************************************//**

\class NyqControl
\brief A control on a NyquistDialog.

*//*******************************************************************/
#include "NyquistControls.h"

#include "NyquistFormatting.h"
#include "../../ShuttleAutomation.h"
#include <float.h>

void NyquistControls::Visit(
   const Bindings &bindings, ConstSettingsVisitor &visitor) const
{
   auto pBinding = bindings.cbegin();
   for (const auto &ctrl : mControls) {
      auto &binding = *pBinding++;
      double d = binding.val;

      if (d == UNINITIALIZED_CONTROL && ctrl.type != NYQ_CTRL_STRING)
         d = NyquistFormatting::GetCtrlValue(binding.valStr);

      if (ctrl.type == NYQ_CTRL_FLOAT || ctrl.type == NYQ_CTRL_FLOAT_TEXT ||
          ctrl.type == NYQ_CTRL_TIME)
         visitor.Define( d, static_cast<const wxChar*>( ctrl.var.c_str() ),
            (double)0.0, ctrl.low, ctrl.high, 1.0);
      else if (ctrl.type == NYQ_CTRL_INT || ctrl.type == NYQ_CTRL_INT_TEXT) {
         int x = d;
         visitor.Define( x, static_cast<const wxChar*>( ctrl.var.c_str() ), 0,
            static_cast<int>(ctrl.low), static_cast<int>(ctrl.high), 1);
         //parms.Write(ctrl.var, (int) d);
      }
      else if (ctrl.type == NYQ_CTRL_CHOICE) {
         // untranslated
         int x = d;
         //parms.WriteEnum(ctrl.var, (int) d, choices);
         visitor.DefineEnum( x, static_cast<const wxChar*>( ctrl.var.c_str() ),
            0, ctrl.choices.data(), ctrl.choices.size() );
      }
      else if (ctrl.type == NYQ_CTRL_STRING || ctrl.type == NYQ_CTRL_FILE) {
         visitor.Define( binding.valStr, ctrl.var,
            wxString{}, ctrl.lowStr, ctrl.highStr );
         //parms.Write(ctrl.var, ctrl.valStr);
      }
   }
}

bool NyquistControls::Save(
   const Bindings &bindings, CommandParameters & parms) const
{
   auto pBinding = bindings.cbegin();
   for (const auto &ctrl : mControls) {
      auto &binding = *pBinding++;
      double d = binding.val;

      if (d == UNINITIALIZED_CONTROL && ctrl.type != NYQ_CTRL_STRING)
      {
         d = NyquistFormatting::GetCtrlValue(binding.valStr);
      }

      if (ctrl.type == NYQ_CTRL_FLOAT || ctrl.type == NYQ_CTRL_FLOAT_TEXT ||
          ctrl.type == NYQ_CTRL_TIME)
      {
         parms.Write(ctrl.var, d);
      }
      else if (ctrl.type == NYQ_CTRL_INT || ctrl.type == NYQ_CTRL_INT_TEXT)
      {
         parms.Write(ctrl.var, (int) d);
      }
      else if (ctrl.type == NYQ_CTRL_CHOICE)
      {
         // untranslated
         parms.WriteEnum(ctrl.var, (int) d,
                         ctrl.choices.data(), ctrl.choices.size());
      }
      else if (ctrl.type == NYQ_CTRL_STRING)
      {
         parms.Write(ctrl.var, binding.valStr);
      }
      else if (ctrl.type == NYQ_CTRL_FILE)
      {
         // Convert the given path string to platform-dependent equivalent
         NyquistFormatting::
         resolveFilePath(const_cast<NyqValue&>(binding).valStr);
         parms.Write(ctrl.var, binding.valStr);
      }
   }

   return true;
}

int NyquistControls::Load(Bindings &bindings,
   const CommandParameters & parms, bool bTestOnly)
{
   int badCount = 0;
   // First pass verifies values
   auto pBinding = bindings.begin();
   for (const auto &ctrl : mControls) {
      auto &binding = *pBinding++;
      bool good = false;

      // This GetCtrlValue code is preserved from former code,
      // but probably is pointless.  The value d isn't used later,
      // and GetCtrlValue does not appear to have important needed
      // side effects.
      if (!bTestOnly) {
         double d = binding.val;
         if (d == UNINITIALIZED_CONTROL && ctrl.type != NYQ_CTRL_STRING)
         {
            d = NyquistFormatting::GetCtrlValue(binding.valStr);
         }
      }

      if (ctrl.type == NYQ_CTRL_FLOAT || ctrl.type == NYQ_CTRL_FLOAT_TEXT ||
         ctrl.type == NYQ_CTRL_TIME)
      {
         double val;
         good = parms.Read(ctrl.var, &val) &&
            val >= ctrl.low &&
            val <= ctrl.high;
         if (good && !bTestOnly)
            binding.val = val;
      }
      else if (ctrl.type == NYQ_CTRL_INT || ctrl.type == NYQ_CTRL_INT_TEXT)
      {
         int val;
         good = parms.Read(ctrl.var, &val) &&
            val >= ctrl.low &&
            val <= ctrl.high;
         if (good && !bTestOnly)
            binding.val = (double)val;
      }
      else if (ctrl.type == NYQ_CTRL_CHOICE)
      {
         int val;
         // untranslated
         good = parms.ReadEnum(ctrl.var, &val,
            ctrl.choices.data(), ctrl.choices.size()) &&
            val != wxNOT_FOUND;
         if (good && !bTestOnly)
            binding.val = (double)val;
      }
      else if (ctrl.type == NYQ_CTRL_STRING || ctrl.type == NYQ_CTRL_FILE)
      {
         wxString val;
         good = parms.Read(ctrl.var, &val);
         if (good && !bTestOnly)
            binding.valStr = val;
      }
      else if (ctrl.type == NYQ_CTRL_TEXT)
      {
         // This "control" is just fixed text (nothing to save or restore),
         // Does not count for good/bad counting.
         good = true;
      }
      badCount += !good ? 1 : 0;
   }
   return badCount;
}

wxString NyquistControls::Expression(const Bindings &bindings) const
{
   wxString cmd;
   auto pBinding = bindings.cbegin();
   for (const auto &ctrl : mControls) {
      auto &binding = *pBinding++;
      if (ctrl.type == NYQ_CTRL_FLOAT || ctrl.type == NYQ_CTRL_FLOAT_TEXT ||
          ctrl.type == NYQ_CTRL_TIME) {
         // We use Internat::ToString() rather than "%f" here because we
         // always have to use the dot as decimal separator when giving
         // numbers to Nyquist, whereas using "%f" will use the user's
         // decimal separator which may be a comma in some countries.
         cmd += wxString::Format(wxT("(setf %s %s)\n"),
                                 ctrl.var,
                                 Internat::ToString(binding.val, 14));
      }
      else if (ctrl.type == NYQ_CTRL_INT ||
            ctrl.type == NYQ_CTRL_INT_TEXT ||
            ctrl.type == NYQ_CTRL_CHOICE) {
         cmd += wxString::Format(wxT("(setf %s %d)\n"),
                                 ctrl.var,
                                 (int)(binding.val));
      }
      else if (ctrl.type == NYQ_CTRL_STRING || ctrl.type == NYQ_CTRL_FILE) {
         cmd += wxT("(setf ");
         // restrict variable names to 7-bit ASCII:
         cmd += ctrl.var;
         cmd += wxT(" \"");
         cmd += NyquistFormatting::EscapeString(binding.valStr);
         // unrestricted value will become quoted UTF-8
         cmd += wxT("\")\n");
      }
   }
   return cmd;
}
