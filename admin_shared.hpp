// Codigo compartido por el mod y por gdopt-admin.exe: la tabla de ajustes de Windows y las
// ordenes del teclado. Solo Win32 y la biblioteca estandar. Todo lo que genera ordenes usa
// textos fijos; lo unico variable son identificadores que da Windows y que se filtran antes.
#pragma once
#include <Windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

struct TweakCheck { char kind; const char* path; const char* name; const char* value; }; // d = dword, s = texto, c = contiene
struct WinTweak {
	const char* key; const char* name;
	std::vector<const char*> apply, revert;
	std::vector<TweakCheck> checks;
};
#include "tweaks.inc"

static const char* kDisplayClass = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";

// Devuelve la subclave ("0000", "0001"...) de la grafica NVIDIA, o vacio si no hay ninguna.
inline std::string nvidiaClassKey() {
	for (int i = 0; i < 16; i++) {
		char subBuf[8]; wsprintfA(subBuf, "%04d", i); std::string sub = subBuf;
		HKEY h;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, (std::string(kDisplayClass) + "\\" + sub).c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) continue;
		bool nv = false;
		for (const char* name : {"ProviderName", "DriverDesc"}) {
			char buf[256] = {};
			DWORD size = sizeof(buf) - 1, type = 0;
			if (RegQueryValueExA(h, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS && type == REG_SZ) {
				std::string v = buf;
				for (auto& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				if (v.find("NVIDIA") != std::string::npos) nv = true;
			}
		}
		RegCloseKey(h);
		if (nv) return sub;
	}
	return {};
}

// Las ordenes y comprobaciones de NVIDIA traen la subclave "0000"; se cambia por la real.
inline std::string fixNvPath(std::string s) {
	static const std::string marker = "{4d36e968-e325-11ce-bfc1-08002be10318}\\0000";
	auto pos = s.find(marker);
	if (pos == std::string::npos) return s;
	auto key = nvidiaClassKey();
	if (!key.empty()) s.replace(pos + marker.size() - 4, 4, key);
	return s;
}

inline std::string base64(const unsigned char* data, size_t len) {
	static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = data[i] << 16;
		if (i + 1 < len) v |= data[i + 1] << 8;
		if (i + 2 < len) v |= data[i + 2];
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += i + 1 < len ? tbl[(v >> 6) & 63] : '=';
		out += i + 2 < len ? tbl[v & 63] : '=';
	}
	return out;
}

// Nucleo reservado para el teclado: el ultimo nucleo fisico rapido (nunca el que contiene la CPU 0).
// full = todos sus hilos; first = solo el primero (el gemelo SMT se deja vacio).
inline bool reservedCore(DWORD_PTR systemMask, DWORD_PTR& full, DWORD_PTR& first) {
	full = first = 0;
	DWORD len = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
	if (len == 0) return false;
	std::vector<char> buf(len);
	if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) return false;
	int best = -1;
	for (int pass = 0; pass < 2; pass++) {
		for (DWORD off = 0; off < len;) {
			auto cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
			if (cur->Size == 0) break;
			if (cur->Relationship == RelationProcessorCore && cur->Processor.GroupCount > 0 && cur->Processor.GroupMask[0].Group == 0) {
				DWORD_PTR core = cur->Processor.GroupMask[0].Mask & systemMask;
				int cls = cur->Processor.EfficiencyClass;
				if (core && !(core & 1)) {
					if (pass == 0) best = std::max(best, cls);
					else if (cls == best && core > full) full = core;
				}
			}
			off += cur->Size;
		}
	}
	first = full & (~full + 1);
	return full != 0;
}

inline int maskBits(DWORD_PTR m) { int n = 0; for (; m; m &= m - 1) n++; return n; }


// ---- Nucleo propio para el teclado (experimental) ----
// Se busca el controlador USB (dispositivo PCI) del que cuelga cada teclado y se le fija la
// interrupcion al nucleo reservado, con prioridad alta. Lo unico que entra en las ordenes es el
// identificador que da Windows, y solo si tiene caracteres normales.
inline bool safeInstanceId(const std::string& id) {
	if (id.size() < 8 || id.size() > 180 || id.rfind("PCI\\", 0) != 0) return false;
	for (char c : id) {
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '&' || c == '_' || c == '\\' || c == '.' || c == '-';
		if (!ok) return false;
	}
	return id.find("\\\\") == std::string::npos && id.back() != '\\';
}

inline std::vector<std::string> keyboardControllers() {
	std::vector<std::string> out;
	HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_KEYBOARD, nullptr, nullptr, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) return out;
	SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
		DEVINST cur = dev.DevInst;
		bool sawUsb = false;
		for (int depth = 0; depth < 24; depth++) {
			char id[MAX_DEVICE_ID_LEN + 1] = {};
			if (CM_Get_Device_IDA(cur, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
			std::string s = id;
			for (auto& c : s) if (c >= 'a' && c <= 'z') c -= 32;
			if (s.rfind("USB\\", 0) == 0) sawUsb = true;
			if (s.rfind("PCI\\", 0) == 0) {
				if (sawUsb && safeInstanceId(s) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
				break;
			}
			DEVINST parent = 0;
			if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) break;
			cur = parent;
		}
	}
	SetupDiDestroyDeviceInfoList(set);
	return out;
}

// Camino de cada teclado USB: del dispositivo hasta el controlador de la placa.
struct KbPath {
	std::string usbId, name;        // el dispositivo USB (uno por teclado) y su nombre
	std::vector<std::string> nodes; // todos los identificadores del camino, en mayusculas
	int hubs = 0;                   // concentradores intermedios (sin contar el raiz)
};

// identificador que puede ir dentro de comillas simples de PowerShell sin riesgo
inline bool safeDeviceId(const std::string& id) {
	if (id.size() < 5 || id.size() > 200) return false;
	for (char c : id) {
		bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '&' || c == '_' || c == '\\' || c == '.' || c == '-' || c == '{' || c == '}' || c == '#' || c == '+';
		if (!ok) return false;
	}
	return true;
}

inline std::vector<KbPath> keyboardPaths() {
	static const DEVPROPKEY kBusDesc = {{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};
	std::vector<KbPath> out;
	HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_KEYBOARD, nullptr, nullptr, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) return out;
	SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
		KbPath p;
		DEVINST cur = dev.DevInst;
		bool reached = false;
		for (int depth = 0; depth < 24; depth++) {
			char id[MAX_DEVICE_ID_LEN + 1] = {};
			if (CM_Get_Device_IDA(cur, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
			std::string s = id;
			for (auto& c : s) if (c >= 'a' && c <= 'z') c -= 32;
			p.nodes.push_back(s);
			bool usb = s.rfind("USB\\", 0) == 0;
			if (usb && s.rfind("USB\\ROOT_HUB", 0) != 0) {
				char svc[64] = {}; ULONG len = sizeof(svc);
				CM_Get_DevNode_Registry_PropertyA(cur, CM_DRP_SERVICE, nullptr, svc, &len, 0);
				std::string sv = svc;
				for (auto& c : sv) if (c >= 'a' && c <= 'z') c -= 32;
				if (sv.rfind("USBHUB", 0) == 0) {
					p.hubs++;
				} else if (p.usbId.empty() && s.find("&MI_") == std::string::npos) {
					p.usbId = s;
					wchar_t w[200] = {}; ULONG size = sizeof(w) - sizeof(wchar_t); DEVPROPTYPE type = 0;
					if (CM_Get_DevNode_PropertyW(cur, &kBusDesc, &type, reinterpret_cast<PBYTE>(w), &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_STRING) {
						for (wchar_t* c = w; *c; c++) p.name += (*c >= 32 && *c < 127 && *c != '<' && *c != '>') ? static_cast<char>(*c) : '?';
					}
				}
			}
			if (s.rfind("PCI\\", 0) == 0) { reached = true; break; }
			DEVINST parent = 0;
			if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) break;
			cur = parent;
		}
		if (!reached || p.usbId.empty()) continue;
		auto it = std::find_if(out.begin(), out.end(), [&](const KbPath& o) { return o.usbId == p.usbId; });
		if (it == out.end()) out.push_back(p);
		else for (auto& n : p.nodes) if (std::find(it->nodes.begin(), it->nodes.end(), n) == it->nodes.end()) it->nodes.push_back(n);
	}
	SetupDiDestroyDeviceInfoList(set);
	return out;
}


inline std::string kbdPolicyKey(const std::string& id) {
	return "SYSTEM\\CurrentControlSet\\Enum\\" + id + "\\Device Parameters\\Interrupt Management\\Affinity Policy";
}


inline std::string lockMemCommandFor(const std::string& sid, bool add) {
	if (sid.empty() || sid.find_first_not_of("S-0123456789") != std::string::npos) return {};
	std::string ps =
		"$sid='" + sid + "'\n"
		"$add=" + (add ? "$true" : "$false") + "\n"
		"$t=Join-Path $env:TEMP 'gdopt-sec.inf'\n$db=Join-Path $env:TEMP 'gdopt-sec.sdb'\n"
		"secedit /export /cfg $t /areas USER_RIGHTS | Out-Null\n"
		"$c=@(Get-Content $t -Encoding Unicode)\n"
		"$found=$false\n"
		"$c=@($c | ForEach-Object { if($_ -match '^SeLockMemoryPrivilege\\s*=\\s*(.*)$'){ $found=$true; "
		"$l=@($Matches[1].Split(',') | ForEach-Object {$_.Trim()} | Where-Object {$_ -and $_ -ne ('*'+$sid)}); "
		"if($add){$l+=('*'+$sid)}; 'SeLockMemoryPrivilege = '+($l -join ',') } else { $_ } })\n"
		"if(-not $found -and $add){ $c=@($c | ForEach-Object { $_; if($_ -eq '[Privilege Rights]'){ 'SeLockMemoryPrivilege = *'+$sid } }) }\n"
		"Set-Content $t $c -Encoding Unicode\n"
		"secedit /configure /db $db /cfg $t /areas USER_RIGHTS | Out-Null\n"
		"Remove-Item $t,$db -ErrorAction SilentlyContinue\n";
	std::wstring wide(ps.begin(), ps.end());
	return "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand "
		+ base64(reinterpret_cast<const unsigned char*>(wide.data()), wide.size() * sizeof(wchar_t));
}


// Orden que quita (o devuelve) "Permitir que el equipo apague este dispositivo" en esos dispositivos.
// Script fijo; solo entran identificadores que da Windows, ya filtrados.
inline std::string kbdPmCommand(const std::vector<std::string>& ids, bool enable) {
	std::string list;
	int count = 0;
	for (auto& id : ids) {
		if (!safeDeviceId(id) || count >= 24) continue;
		list += (count++ ? ",'" : "'") + id + "'";
	}
	if (!count) return {};
	std::string ps =
		"$ids=@(" + list + ")\n"
		"$en=" + (enable ? "$true" : "$false") + "\n"
		"Get-CimInstance -Namespace root\\wmi -ClassName MSPower_DeviceEnable | ForEach-Object { $n=$_.InstanceName.ToUpper(); "
		"foreach($i in $ids){ if($n.StartsWith($i+'_')){ Set-CimInstance -InputObject $_ -Property @{Enable=$en}; break } } }\n";
	std::wstring wide(ps.begin(), ps.end());
	std::string cmd = "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand "
		+ base64(reinterpret_cast<const unsigned char*>(wide.data()), wide.size() * sizeof(wchar_t));
	return cmd.size() < 7800 ? cmd : std::string();
}

inline std::vector<std::string> keyboardPathIds() {
	std::vector<std::string> ids;
	for (auto& p : keyboardPaths())
		for (auto& n : p.nodes)
			if (std::find(ids.begin(), ids.end(), n) == ids.end()) ids.push_back(n);
	return ids;
}

inline void kbdCoreDelCmds(const std::string& id, std::vector<std::string>& out) {
	if (!safeInstanceId(id)) return;
	for (const char* v : {"DevicePolicy", "AssignmentSetOverride", "DevicePriority"})
		out.push_back("reg delete \"HKLM\\" + kbdPolicyKey(id) + "\" /v " + v + " /f");
}

inline void kbdCoreAddCmds(const std::string& id, DWORD_PTR first, std::vector<std::string>& out) {
	if (!safeInstanceId(id) || !first) return;
	static const char* hx = "0123456789ABCDEF";
	std::string hex;
	for (int i = 0; i < 8; i++) { unsigned b = static_cast<unsigned>((static_cast<unsigned long long>(first) >> (8 * i)) & 0xFF); hex += hx[b >> 4]; hex += hx[b & 15]; }
	std::string k = "reg add \"HKLM\\" + kbdPolicyKey(id) + "\"";
	out.push_back(k + " /v DevicePolicy /t REG_DWORD /d 4 /f");
	out.push_back(k + " /v AssignmentSetOverride /t REG_BINARY /d " + hex + " /f");
	out.push_back(k + " /v DevicePriority /t REG_DWORD /d 3 /f");
}
