' ============================================================
'  StartIDFCServer.vbs
'  Find the latest ESP-IDF 5.5.x activation script under
'  C:\Espressif\tools (Microsoft.v5.5.*.PowerShell_profile.ps1)
'  and start the IDF server (idfc.exe --server --idfenv ...).
'  If none is found, show a message and wait for a key press.
'
'  Run via StartIDFCServer.cmd (cscript //nologo) so that
'  console output and WScript.StdIn are available.
' ============================================================
Option Explicit

' ------------------------------------------------------------------
' Configuration - edit TOOLS_DIR if your ESP-IDF lives elsewhere
' ------------------------------------------------------------------
Const TOOLS_DIR    = "C:\Espressif\tools"
Const FILE_PATTERN = "^Microsoft\.v5\.5\..+\.PowerShell_profile\.ps1$"

Dim fso, shell
Set fso   = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

' ------------------------------------------------------------------
' Find the activation script with the highest version number
' ------------------------------------------------------------------
Dim bestFile, bestVersion, folder, file, ver, reMatch
bestFile    = ""
bestVersion = ""

' Regexp-based file name match (VBScript "Like" operator is unreliable
' on some systems, so we use RegExp for the pattern test)
Set reMatch = New RegExp
reMatch.Pattern    = FILE_PATTERN
reMatch.IgnoreCase = True

If fso.FolderExists(TOOLS_DIR) Then
    Set folder = fso.GetFolder(TOOLS_DIR)
    For Each file In folder.Files
        If reMatch.Test(file.Name) Then
            ver = ExtractVersion(file.Name)
            If ver <> "" Then
                If bestVersion = "" Or CompareVersions(ver, bestVersion) > 0 Then
                    bestVersion = ver
                    bestFile    = file.Path
                End If
            End If
        End If
    Next
End If

If bestFile = "" Then
    MsgBox "The installation of IDF 5.5.x is not found."
    
Else

	Dim exePath
	exePath = fso.GetParentFolderName(WScript.ScriptFullName) & "\idfc.exe"

	Dim exitCode
	exitCode = shell.Run("""" & exePath & """ --server --idfenv """ & bestFile & """", 1, True)
	WScript.Quit exitCode
    
End If



' ==================================================================
' Helper functions
' ==================================================================

' Extract "5.5.x" from "Microsoft.v5.5.x.PowerShell_profile.ps1";
' returns "" if the name does not match.
Function ExtractVersion(ByVal fileName)
    Dim re, m
    Set re = New RegExp
    re.Pattern    = "Microsoft\.v([0-9.]+)\.PowerShell_profile\.ps1"
    re.IgnoreCase = True
    Set m = re.Execute(fileName)
    If m.Count > 0 Then
        ExtractVersion = m(0).SubMatches(0)
    Else
        ExtractVersion = ""
    End If
End Function

' Compare dotted numeric versions: 1 if a > b, 0 if equal, -1 if a < b
Function CompareVersions(ByVal a, ByVal b)
    Dim pa, pb, i, na, nb
    pa = Split(a, ".")
    pb = Split(b, ".")
    For i = 0 To UBound(pa)
        If i > UBound(pb) Then
            CompareVersions = 1
            Exit Function
        End If
        na = CInt(pa(i))
        nb = CInt(pb(i))
        If na > nb Then
            CompareVersions = 1
            Exit Function
        ElseIf na < nb Then
            CompareVersions = -1
            Exit Function
        End If
    Next
    If UBound(pb) > UBound(pa) Then
        CompareVersions = -1
    Else
        CompareVersions = 0
    End If
End Function
