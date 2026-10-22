#pragma once
class wxWindow;
class wxString;
namespace Slic3r { namespace GUI {
void show_hotend_thermal_response(wxWindow* parent);
void show_hotend_thermal_response_file(wxWindow* parent, const wxString& path);
}}
