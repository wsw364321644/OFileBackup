#pragma once
enum class EFileBackupError
{
    FBE_OK,
    FBE_FILE_NOT_EXIST,
    FBE_FILE_OP_ERROR,
    FBE_FILE_ALREADY_EXIST,
    FBE_OPTION_ERROR,
    FBE_INTERNAL_ERROR,
};