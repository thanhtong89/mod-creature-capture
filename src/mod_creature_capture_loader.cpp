/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU AGPL v3 license
 */

// Add all script registration functions
void AddSC_mod_creature_capture();

// This is the entry point called by the module loader
void Addmod_creature_captureScripts()
{
    AddSC_mod_creature_capture();
}
