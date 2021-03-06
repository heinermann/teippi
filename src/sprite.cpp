#include "sprite.h"

#include "resolution.h"
#include <algorithm>
#include <array>
#include "offsets.h"
#include "unit.h"
#include "image.h"
#include "patchmanager.h"
#include "lofile.h"
#include "limits.h"
#include "game.h"
#include "yms.h"
#include "selection.h"
#include "warn.h"
#include "rng.h"
#include "perfclock.h"

#include "log.h"

LoneSpriteSystem *lone_sprites;

uint32_t Sprite::next_id = 1;
uint32_t Sprite::count = 0;
uint32_t Sprite::draw_order_limit = 0x22DD0; // 0x150 * 0x6a4 / 0x4 (that is whole unit array)
Sprite **Sprite::draw_order = (Sprite **)bw::units.v();
int Sprite::draw_order_amount;

#ifdef SYNC
void *Sprite::operator new(size_t size)
{
    auto ret = new uint8_t[size];
    if (SyncTest)
        ScrambleStruct(ret, size);
    return ret;
}
#endif


Sprite::Sprite()
{
    index = 0;
}

void Sprite::PackIds()
{
    next_id = 1;

    for (int i = 0; i < *bw::map_height_tiles; i++)
    {
        for (Sprite *sprite : bw::horizontal_sprite_lines[i])
        {
            sprite->id = next_id++;
        }
    }
}

void Sprite::AddToHlines()
{
    int y_tile = position.y / 32;
    if (y_tile < 0)
        y_tile = 0;
    else if (y_tile >= *bw::map_height_tiles)
        y_tile = *bw::map_height_tiles - 1;

    if (!bw::horizontal_sprite_lines[y_tile])
    {
        bw::horizontal_sprite_lines[y_tile] = this;
        bw::horizontal_sprite_lines_rev[y_tile] = this;
        list.next = nullptr;
        list.prev = nullptr;
    }
    else
    {
        if (bw::horizontal_sprite_lines[y_tile] == bw::horizontal_sprite_lines_rev[y_tile])
            bw::horizontal_sprite_lines_rev[y_tile] = this;
        list.prev = bw::horizontal_sprite_lines[y_tile];
        list.next = list.prev->list.next;
        if (list.next)
            list.next->list.prev = this;
        list.prev->list.next = this;
    }
}

Sprite *Sprite::RawAlloc()
{
    Sprite *sprite;
    sprite = new Sprite;
    sprite->id = next_id++;
    if (next_id == 0)
    {
        PackIds();
    }
    count++;
    if (count > draw_order_limit)
    {
        draw_order_limit *= 2;
        if (draw_order == (Sprite **)bw::units.v())
            draw_order = (Sprite **)malloc(draw_order_limit * sizeof(Sprite *));
        else
            draw_order = (Sprite **)realloc(draw_order, draw_order_limit * sizeof(Sprite *));
    }
    return sprite;
}

Sprite *Sprite::AllocateBase(int sprite_id, const Point &pos, int player)
{
    Sprite *sprite = RawAlloc();

    sprite->main_image = nullptr;
    sprite->first_overlay = nullptr;
    sprite->last_overlay = nullptr;
    if (InitializeSprite(sprite, sprite_id, pos.x, pos.y, player) == 0)
    {
        // Remove() can't be called as AddToHlines() hasn't been called
        // So lazy way of doing things
        Assert(!sprite->first_overlay);
        count--;
        return nullptr;
    }
    else
    {
        //debug_log->Log("Init main image: ID %x, addr %p, parent %p (%d)\n", sprite->main_image->image_id, sprite->main_image, sprite->main_image->parent, sprite->main_image->parent == sprite);

        sprite->AddToHlines();
        return sprite;
    }
}

Sprite *Sprite::Allocate(int sprite_id, const Point &pos, int player)
{
    return AllocateBase(sprite_id, pos, player);
}

Sprite *LoneSpriteSystem::AllocateLone(int sprite_id, const Point &pos, int player)
{
    Sprite *sprite = Sprite::AllocateBase(sprite_id, pos, player);
    if (sprite)
    {
        sprite->container_index = lone_sprites.size();
        lone_sprites.emplace_back(sprite);
    }
    return sprite;
}

Sprite *LoneSpriteSystem::AllocateFow(Sprite *base, int unit_id)
{
    Sprite *sprite = Sprite::AllocateBase(base->sprite_id, base->position, base->player);
    if (!sprite)
        return nullptr;

    sprite->index = unit_id;
    sprite->container_index = fow_sprites.size();
    fow_sprites.emplace_back(sprite);

    for (Image *img = sprite->first_overlay, *next; img; img = next)
    {
        next = img->list.next;
        img->SingleDelete();
    }
    sprite->main_image = nullptr;
    for (Image *img : base->last_overlay)
    {
        if (img->drawfunc == Image::HpBar || img->drawfunc == Image::SelectionCircle)
            continue;
        Image *current = AddOverlayNoIscript(sprite, img->image_id, img->x_off, img->y_off, img->direction);
        current->frame = img->frame;
        current->frameset = img->frameset;
        current->SetFlipping(img->flags & 0x2);
        PrepareDrawImage(current);
        if (img == base->main_image)
            sprite->main_image = current;
    }
    return sprite;
}

Sprite *Sprite::SpawnLoneSpriteAbove(int sprite_id)
{
    Sprite *sprite = lone_sprites->AllocateLone(sprite_id, position, player);
    if (sprite)
    {
        sprite->elevation = elevation + 1;
        sprite->UpdateVisibilityPoint();
    }
    return sprite;
}

void Sprite::Remove()
{
    //debug_log->Log("Delete sprite %p, type %x\n", this, sprite_id);
    Image *img, *next_img = first_overlay;
    while (next_img)
    {
        img = next_img;
        next_img = img->list.next;
        img->SingleDelete();
    }

    int y_tile = position.y / 32;
    if (y_tile >= *bw::map_height_tiles)
        y_tile = *bw::map_height_tiles - 1;

    if (list.next)
        list.next->list.prev = list.prev;
    else
    {
        Assert(bw::horizontal_sprite_lines_rev[y_tile] == this);
        bw::horizontal_sprite_lines_rev[y_tile] = list.prev;
    }
    if (list.prev)
        list.prev->list.next = list.next;
    else
    {
        Assert(bw::horizontal_sprite_lines[y_tile] == this);
        bw::horizontal_sprite_lines[y_tile] = list.next;
    }

    count--;
}

void Sprite::SingleDelete()
{
    Remove();
}

void LoneSpriteSystem::DeleteAll()
{
    lone_sprites.clear();
    fow_sprites.clear();
}

void Sprite::DeleteAll()
{
    next_id = 1;
    count = 0;

    // Might as well free
    if (draw_order != (Sprite **)bw::units.v())
    {
        free(draw_order);
        draw_order = (Sprite **)bw::units.v();
        draw_order_limit = 0x22DD0;
    }
    draw_order_amount = 0;
}

static bool SpritePtrCompare(Sprite *a, Sprite *b)
{
    if (a->sort_order == b->sort_order)
        return a->id < b->id;
    return a->sort_order < b->sort_order;
}

uint32_t Sprite::GetZCoord() const
{
    int y = 0;
    if (elevation <= 4)
        y = position.y;
    return (elevation << 0x1b) | (y << 0xb) | (flags & 0x10); // There would be 11 bits space after flag 0x10 << 6
}

void Sprite::CreateDrawSpriteList()
{
    // todo other makedrawlist stuff
    int first_y = *bw::screen_pos_y_tiles - 4;
    int last_y = first_y + resolution::game_height_tiles + 8;
    if (first_y < 0)
        first_y = 0;
    if (last_y >= *bw::map_height_tiles)
        last_y = *bw::map_height_tiles - 1;

    uint8_t vision_mask;
    if (all_visions)
        vision_mask = 0xff;
    else if (IsReplay())
        vision_mask = *bw::replay_visions;
    else
        vision_mask = *bw::player_visions;

    draw_order_amount = 0;
    while (first_y <= last_y)
    {
        for (Sprite *sprite : bw::horizontal_sprite_lines[first_y])
        {
            if (sprite->visibility_mask & vision_mask)
            {
                draw_order[draw_order_amount++] = sprite;
                sprite->sort_order = sprite->GetZCoord();
            }
            PrepareDrawSprite(sprite); // Has to be done outside the loop or cursor marker sprite will bug
        }
        first_y++;
    }
    std::sort(draw_order, draw_order + draw_order_amount, SpritePtrCompare);
}

void Sprite::CreateDrawSpriteListFullRedraw()
{
    // todo other makedrawlist stuff
    int first_y = *bw::screen_pos_y_tiles - 4;
    int last_y = first_y + resolution::game_height_tiles + 8;
    if (first_y < 0)
        first_y = 0;
    if (last_y >= *bw::map_height_tiles)
        last_y = *bw::map_height_tiles - 1;

    uint8_t vision_mask;
    if (all_visions)
        vision_mask = 0xff;
    else if (IsReplay())
        vision_mask = *bw::replay_visions;
    else
        vision_mask = *bw::player_visions;

    draw_order_amount = 0;
    while (first_y <= last_y)
    {
        for (Sprite *sprite : bw::horizontal_sprite_lines[first_y])
        {
            if (sprite->visibility_mask & vision_mask)
            {
                draw_order[draw_order_amount++] = sprite;
                sprite->sort_order = sprite->GetZCoord();
            }
        }
        first_y++;
    }
    std::sort(draw_order, draw_order + draw_order_amount, SpritePtrCompare);
}

void Sprite::DrawSprites()
{
    PerfClock clock;
    std::for_each(draw_order, draw_order + draw_order_amount, DrawSprite);
    auto time = clock.GetTime();
    if (!*bw::is_paused && time > 12.0)
        perf_log->Log("DrawSprites %f ms\n", time);
}

void Sprite::UpdateVisibilityArea()
{
    if (IsHidden())
        return;
    int x_tile = (position.x - width / 2) / 32;
    int y_tile = (position.y - height / 2) / 32;
    int w_tile = (width + 31) / 32;
    int h_tile = (height + 31) / 32;
    if (x_tile < 0)
    {
        if (x_tile + w_tile <= 0)
            return;
        w_tile += x_tile;
        x_tile = 0;
    }
    if (y_tile < 0)
    {
        if (y_tile + h_tile <= 0)
            return;
        h_tile += y_tile;
        y_tile = 0;
    }
    int map_width = *bw::map_width_tiles, map_height = *bw::map_height_tiles;
    if (x_tile + w_tile > map_width && map_width <= x_tile)
        return;
    if (y_tile + h_tile > map_height && map_height <= y_tile)
        return;

    uint8_t visibility = GetAreaVisibility(x_tile, y_tile, w_tile, h_tile);
    if (visibility != visibility_mask)
        SetVisibility(this, visibility);

    // orig func returns shit but not necessary now
}

bool Sprite::UpdateVisibilityPoint()
{
    if (IsHidden())
        return false;
    int new_visibility = ~((*bw::map_tile_flags)[*bw::map_width_tiles * (position.y / 32) + (position.x / 32)]);
    int old_local_visibility = visibility_mask & *bw::player_visions;
    if (new_visibility != visibility_mask)
    {
        SetVisibility(this, new_visibility);
        if (old_local_visibility && !(visibility_mask & *bw::player_visions)) // That is, if vision was lost
            return true;
    }
    return false;
}

void UpdateDoodadVisibility(Sprite *sprite)
{
    if (sprite->IsHidden())
        return;
    int x_tile = (sprite->position.x - sprite->width / 2) / 32;
    int y_tile = (sprite->position.y - sprite->height / 2) / 32;
    int width = (sprite->width + 31) / 32;
    int height = (sprite->height + 31) / 32;
    if ((x_tile < 0 && x_tile + width <= 0) || (y_tile < 0 && y_tile + height <= 0))
        return;
    int map_width = *bw::map_width_tiles, map_height = *bw::map_height_tiles;
    if (x_tile + width <= map_width || map_width - x_tile > 0)
        return;
    if (y_tile + height <= map_height || map_height - y_tile > 0)
        return;

    uint8_t visibility;
    if (*bw::is_replay && *bw::replay_show_whole_map)
        visibility = 0xff;
    else
        visibility = GetAreaExploration(x_tile, y_tile, width, height);

    if (visibility != sprite->visibility_mask)
        SetVisibility(sprite, visibility);

    // Orig func returns shit but not necessary now
}

Sprite::ProgressFrame_C ProgressLoneSpriteFrame(Sprite *sprite)
{
    if (sprite->sprite_id > 0x81 && (sprite->sprite_id < 0x182 || sprite->sprite_id > 0x1e0))
        sprite->UpdateVisibilityArea();
    else
        UpdateDoodadVisibility(sprite);
    return sprite->ProgressFrame(IscriptContext(), main_rng);
}

bool ProgressLoneSprite2Frame(Sprite *sprite)
{
    DrawTransmissionSelectionCircle(sprite, bw::self_alliance_colors[sprite->player]);
    int place_width = units_dat_placement_box[2 * sprite->index];
    int place_height = units_dat_placement_box[2 * sprite->index + 1];
    int width = (place_width + 31) / 32;
    int height = (place_height + 31) / 32;
    int x = (sprite->position.x - place_width / 2) / 32;
    int y = (sprite->position.y - place_height / 2) / 32;
    if (!IsCompletelyHidden(x, y, width, height))
    {
        RemoveSelectionCircle(sprite);
        return false;
    }
    return true;
}

void LoneSpriteSystem::ProgressFrames()
{
    for (ptr<Sprite> &sprite : lone_sprites.SafeIter())
    {
        for (auto &cmd : ProgressLoneSpriteFrame(sprite.get()))
        {
            if (cmd.opcode == IscriptOpcode::End)
            {
                sprite->Remove();
                lone_sprites.back()->container_index = sprite->container_index;
                lone_sprites.erase_at(sprite->container_index);
            }
            else
                Warning("LoneSpriteSystem::ProgressFrames did not handle iscript command %x for sprite %x",
                       cmd.opcode, sprite->sprite_id);
        }
    }
    for (ptr<Sprite> &sprite : fow_sprites.SafeIter())
    {
        if (ProgressLoneSprite2Frame(sprite.get()) == false)
        {
            sprite->Remove();
            fow_sprites.back()->container_index = sprite->container_index;
            fow_sprites.erase_at(sprite->container_index);
        }
    }
}

void DrawMinimapUnits()
{
    Surface *previous_canvas = *bw::current_canvas;
    *bw::current_canvas = &*bw::minimap_surface;
    *bw::minimap_dot_count = 0;
    *bw::minimap_dot_checksum = 0;
    int local_player = *bw::local_player_id;
    int replay = *bw::is_replay;
    int orig_visions = *bw::player_visions;
    if (all_visions)
        *bw::player_visions = 0xff;
    for (int i = 11; i > 7; i--)
    {
        DrawNeutralMinimapUnits(i);
    }
    for (int i = 7; i >= 0; i--)
    {
        if (replay || i != local_player)
            bw::funcs::DrawMinimapUnits(i);
    }
    if (!replay)
        DrawOwnMinimapUnits(local_player);

    *bw::player_visions = orig_visions;

    for (ptr<Sprite> &sprite : lone_sprites->fow_sprites)
    {
        if (sprite->index < 0xcb || sprite->index > 0xd5)
        {
            int place_width = units_dat_placement_box[2 * sprite->index];
            int place_height = units_dat_placement_box[2 * sprite->index + 1];
            if (replay)
            {
                int width = (place_width + 31) / 32;
                int height = (place_height + 31) / 32;
                int x = (sprite->position.x - place_width / 2) / 32;
                int y = (sprite->position.y - place_height / 2) / 32;
                if (IsCompletelyUnExplored(x, y, width, height))
                    continue;
            }
            int color;
            if (sprite->sprite_id == 0x113 || sprite->sprite_id == 0x117 || sprite->sprite_id == 0x118 || sprite->sprite_id == 0x119)
                color = *bw::minimap_resource_color;
            else
            {
                if (*bw::minimap_color_mode)
                {
                    if (bw::alliances[local_player * Limits::Players + sprite->player])
                        color = *bw::ally_minimap_color;
                    else
                        color = *bw::enemy_minimap_color;
                }
                else
                    color = bw::player_minimap_color[sprite->player];
            }
            DrawMinimapDot(color, sprite->position.x, sprite->position.y, place_width, place_height, 1);
            (*bw::minimap_dot_count)--;
        }
    }
    *bw::current_canvas = previous_canvas;
}

Sprite *Sprite::FindFowTarget(int x, int y)
{
    for (ptr<Sprite> &sprite : lone_sprites->fow_sprites)
    {
        if (Unit::IsClickable(sprite->index))
        {
            if ((unsigned int)((sprite->position.x + units_dat_placement_box[sprite->index * 2] / 2) - x) < sprite->width)
            {
                if ((unsigned int)((sprite->position.y + units_dat_placement_box[sprite->index * 2 + 1] / 2) - y) < sprite->height)
                    return sprite.get();
            }
        }
    }
    return 0;
}

void Sprite::MarkHealthBarDirty()
{
    if (~flags & 0x8)
        return;
    for (Image *img : first_overlay)
    {
        if (img->drawfunc == 0xa)
        {
            img->flags |= 0x1;
            return;
        }
    }
}

Sprite::ProgressFrame_C Sprite::IscriptToIdle(const IscriptContext &ctx, Rng *rng)
{
    switch (main_image->iscript.animation)
    {
        case IscriptAnim::GndAttkInit:
        case IscriptAnim::GndAttkRpt:
            return SetIscriptAnimation(IscriptAnim::GndAttkToIdle, true, ctx, rng);
        break;
        case IscriptAnim::AirAttkInit:
        case IscriptAnim::AirAttkRpt:
            return SetIscriptAnimation(IscriptAnim::AirAttkToIdle, true, ctx, rng);
        break;
        case IscriptAnim::AlmostBuilt:
            if (sprite_id == SCV || sprite_id == Drone || sprite_id == Probe)
                return SetIscriptAnimation(IscriptAnim::GndAttkToIdle, true, ctx, rng);
        break;
        case IscriptAnim::Special1:
            if (sprite_id == Medic)
                return SetIscriptAnimation(IscriptAnim::Idle, true, ctx, rng);
        break;
        case IscriptAnim::CastSpell:
            return SetIscriptAnimation(IscriptAnim::Idle, true, ctx, rng);
        break;
    }
    return ProgressFrame_C(nullptr, -1, ctx, rng);
}

void Sprite::SetFlipping(bool set)
{
    for (Image *img : first_overlay)
    {
        img->SetFlipping(set);
    }
}

void Sprite::AddMultipleOverlaySprites(int overlay_type, int count, int sprite_id, int base, bool flip)
{
    LoFile lo = LoFile::GetOverlay(main_image->image_id, overlay_type);
    for (int i = 0; i < count; i++)
    {
        Point pos = lo.GetPosition(main_image, i + base);
        if (pos.IsValid())
        {
            Sprite *sprite = lone_sprites->AllocateLone(sprite_id + i, pos, player);
            if (sprite)
            {
                sprite->elevation = elevation + 1;
                sprite->UpdateVisibilityPoint();
                if (flip)
                    sprite->SetFlipping(true);
            }
        }
    }
}

void DrawCursorMarker()
{
    if (*bw::draw_cursor_marker)
    {
        Sprite *marker = *bw::cursor_marker;
        for (Image *img : marker->first_overlay)
            PrepareDrawImage(img);
        DrawSprite(marker);
        for (Image *img : marker->first_overlay)
            img->flags |= 0x1;
    }
}

void ShowCursorMarker(int x, int y)
{
    Sprite *marker = *bw::cursor_marker;
    MoveSprite(marker, x, y);
    auto cmds = marker->SetIscriptAnimation(IscriptAnim::GndAttkInit, true);
    if (!Empty(cmds))
        Warning("Iscript for the cursor marker has nonmeaningful commands");
    *bw::draw_cursor_marker = 1;
}

void ShowRallyTarget(Unit *unit)
{

    int x = unit->rally.position.x, y = unit->rally.position.y;
    if ((x || y) && (unit->position != unit->rally.position))
    {
        Sprite *fow = Sprite::FindFowTarget(x, y);
        if (fow)
            fow->selection_flash_timer = 0x1f;
        else
            ShowCursorMarker(x, y);
    }
}

void ShowRallyTarget_Hook()
{
    REG_EAX(Unit *, unit);
    ShowRallyTarget(unit);
}

Sprite *ShowCommandResponse(int x, int y, Sprite *alternate)
{
    if (Sprite *fow = Sprite::FindFowTarget(x, y))
    {
        fow->selection_flash_timer = 0x1f;
        return fow;
    }
    else if (alternate)
    {
        alternate->selection_flash_timer = 0x1f;
        return 0;
    }
    else
    {
        ShowCursorMarker(x, y);
        return 0;
    }
}

Sprite *Sprite::Spawn(Image *spawner, uint16_t sprite_id, const Point &pos, int elevation_level)
{
    int x = pos.x + spawner->x_off + spawner->parent->position.x;
    int y = pos.y + spawner->y_off + spawner->parent->position.y;
    Sprite *sprite = lone_sprites->AllocateLone(sprite_id, Point(x, y), 0);
    if (sprite)
    {
        sprite->elevation = elevation_level;
        sprite->UpdateVisibilityPoint();
    }
    return sprite;
}

void Sprite::SetDirection32(int direction)
{
    for (Image *img : first_overlay)
    {
        SetImageDirection32(img, direction);
    }
}

void Sprite::SetDirection256(int direction)
{
    for (Image *img : first_overlay)
    {
        SetImageDirection256(img, direction);
    }
}

extern "C" void __stdcall SetSpriteDirection(int direction)
{
    REG_EAX(Sprite *, sprite);
    sprite->SetDirection32(direction);
}

Sprite *__stdcall FindBlockingFowResource(int x_tile, int y_tile, int radius)
{
    for (ptr<Sprite> &fow : lone_sprites->fow_sprites)
    {
        if (fow->index == Unit::MineralPatch1 || fow->index == Unit::MineralPatch2
             || fow->index == Unit::MineralPatch3 || fow->index == Unit::VespeneGeyser)
        {
            int w = units_dat_placement_box[2 * fow->index] / 32;
            int h = units_dat_placement_box[2 * fow->index + 1] / 32;
            int x = fow->position.x / 32 - w / 2;
            int y = fow->position.y / 32 - h / 2;
            if (x - radius <= x_tile && x + w + radius > x_tile)
            {
                if (y - radius <= y_tile && y + h + radius > y_tile)
                    return fow.get();
            }
        }
    }
    return 0;
}

void Sprite::RemoveSelectionOverlays()
{
    if (flags & 0x8)
    {
        for (Image *img : first_overlay)
        {
            if (img->drawfunc == Image::HpBar)
            {
                DeleteHealthBarImage(img);
                break;
            }
        }
        flags &= ~0x8;
    }
    if (flags & 0x6)
    {
        for (Image *img : first_overlay)
        {
            if (img->image_id >= Image::FirstDashedSelectionCircle && img->image_id <= Image::LastDashedSelectionCircle)
            {
                DeleteSelectionCircleImage(img);
                break;
            }
        }
        flags &= ~0x6;
    }
    if (flags & 0x1)
    {
        for (Image *img : first_overlay)
        {
            if (img->image_id >= Image::FirstSelectionCircle && img->image_id <= Image::LastSelectionCircle)
            {
                DeleteSelectionCircleImage(img);
                break;
            }
        }
        flags &= ~0x1;
    }
}

void Sprite::RemoveAllSelectionOverlays()
{
    for (unsigned i = 0; i < Limits::ActivePlayers; i++)
    {
        for (Unit *unit : selections[i])
        {
            unit->sprite->RemoveSelectionOverlays();
        }
    }
    // TODO: These may not be added back properly if latency happens
    for (Unit *unit : client_select)
    {
        unit->sprite->RemoveSelectionOverlays();
    }
}

// Bw version has different implementation which may infloop
void Sprite::AddDamageOverlay()
{
    if (main_image == nullptr)
        return;

    LoFile overlay = LoFile::GetOverlay(main_image->image_id, Overlay::Damage);
    if (!overlay.IsValid())
        return;

    // "Upgrade" minor overlay to major if possible
    for (Image *img : first_overlay)
    {
        if (img->image_id >= Image::FirstMinorDamageOverlay && img->image_id <= Image::LastMinorDamageOverlay)
        {
            int variation = img->image_id - Image::FirstMinorDamageOverlay;
            img->SingleDelete();
            Point32 pos = overlay.GetValues(main_image, variation);
            AddOverlayHighest(this, Image::FirstMajorDamageOverlay + variation, pos.x, pos.y, 0);
            return;
        }
    }

    Point32 invalid_pos = Point32(127, 127);
    std::array<int, 22> results;
    auto result_pos = results.begin();
    for (int i = 0; i < 22; i++)
    {
        Point32 pos = overlay.GetValues(main_image, i);
        if (pos == invalid_pos)
            continue;

        int overlay_id = Image::FirstMajorDamageOverlay + i;
        bool found = false;
        for (Image *img : first_overlay)
        {
            if (img->image_id == overlay_id)
            {
                found = true;
                break;
            }
        }
        if (!found)
            *result_pos++ = i;
    }
    Assert(result_pos <= results.end());
    if (result_pos != results.begin())
    {
        int variation = results[main_rng->Rand(result_pos - results.begin())];
        Point32 pos = overlay.GetValues(main_image, variation);
        AddOverlayHighest(this, Image::FirstMinorDamageOverlay + variation, pos.x, pos.y, 0);
    }
}
