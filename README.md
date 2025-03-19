<div align="center">

# OFileBackup

</div>

## Description
OFileBackup contain several application and a lib to help split files.

### OBackupFolder
Split folder with exist file chunks and genarate folder manifest. Need extract and save file chunks with folder manifest another time.

### ORecoverFolder
Use exist file chunks and folder manifest to recover a folder. Also recover from a old folder with it's manifest, and new manifest, and file chunks that old folder does not containe.


### OCompareManifest
Get chunks that left manifest does not containe

### libfilebackup
A lib help split and recover.

## Compile
Use Cmake to genarate project.

## Compatibility 
Only test on windows.