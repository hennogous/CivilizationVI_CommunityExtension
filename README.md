# Civilization VI: Community Extension

## CSC development branch

`csc-dev` is the working branch for the Civ Supply Chains development build.
It descends from WildW's `develop` and brings the tested CSC guidance,
including the Builder finishing scheduler, into one branch. The current native
profile targets the Windows DX11 and Gathering Storm binaries in Steam build
`26595377`. [Relocation ledger](native-relocation.json) · [Port and test report](https://github.com/hennogous/Henno-Mods/blob/main/project/docs/research/mods/csc-ce-steam-26595377-port-20261120.md)

Build Release x64 with the Visual Studio v143 project and Windows SDK. The
verified local setup uses MinHook 1.3.4 for `x64-windows-static`, staged as
`CivilizationVI_CommunityExtension/lib/Release/libMinHook.x64.lib`. Set
`CE_OUTPUT_DIR` to the staging directory. The game loads its installed Gathering
Storm GameCore at runtime. [CE native implementation](CivilizationVI_CommunityExtension/native/README.md)

This mod is currently in beta. Report bugs on the [issue tracker](https://github.com/Wild-W/CivilizationVI_CommunityExtension/issues). You can also contact me directly or on the steam workshop page. I prefer if you do it on the issue tracker though.

[Visit the wiki!](https://github.com/Wild-W/CivilizationVI_CommunityExtension/wiki)

## FAQ

### What does this mod do?
This mod exposes new tools for modders to use by editing Civ VI's base code.

### How does this mod edit the base code?
In essence, the game will load a custom DLL, which then in turn loads the GameCore DLL and makes changes to it. If you want to learn more, join the [Civilization VI Modding Helpline](https://discord.gg/jSVhyBYvZR) and ask me there. (My username is wildw)

### Can you implement X feature?
Probably. Open a feature request on the [issue tracker](https://github.com/Wild-W/CivilizationVI_CommunityExtension/issues). The more precise you are with your request the more likely I'll be able to implement it.

### Is this mod compatible with Mac/Linux?
No. It's possible to port parts of it, but it's just too far out of scope right now.

### Can you fix the asset limit bug?
Probably not. I can *try* at some point, but if it were easy it would have been done by Firaxis already.

## License
This project is distributed under AGPL 3.0. That means that **all** derivative works of this project **must be open source** and **distributed under the same AGPL 3.0 license**. This is to protect the modding community from bad actors and encourage collaboration. The only way this community can grow is if we work together.

## Support
<a href="https://ko-fi.com/wildw">
  <img width="500" src="https://uploads-ssl.webflow.com/5c14e387dab576fe667689cf/5cbed8a4cf61eceb26012821_SupportMe_red.png" />
</a>
