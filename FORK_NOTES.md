# Notas de este fork (LuismaSP89/Cemu)

Este fork contiene tres cambios funcionales respecto a `cemu-project/Cemu`:

1. **Fix: luces/lens flares que atraviesan paredes en ZombiU** (issue upstream
   [cemu-project/Cemu#635](https://github.com/cemu-project/Cemu/issues/635), PR #2047).
2. **Fix: ZombiU no arranca con la actualización v32 (pantalla negra)** — `src/Cafe/Filesystem/fsc.cpp`:
   el listado de un directorio combinado base+update se ordena por nombre, como en consola; antes Cemu
   listaba primero los ficheros de la actualización y el juego indexaba mal sus archivos `.bfz`
   (PR upstream #2048, rama `fix/fsc-merged-dir-order`).
3. **Optimización del fix 1: publicar resultados de queries en el bucle idle del hilo GPU** —
   `src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp` (`LatteCP_readU32Deprc`). Cemu solo escribía en
   memoria los resultados de las occlusion queries terminadas en el vsync virtual o cada 8 `GX2QueryBegin`,
   así que aunque la GPU ya hubiese terminado el juego veía "no listo" y el fix 1 forzaba la sincronización
   completa casi en cada sondeo. Ahora el hilo GPU también los publica cuando se queda sin comandos (1 de cada
   8 iteraciones del bucle idle, porque la comprobación sondea fences de Vulkan), y la sincronización forzada
   queda como fallback poco frecuente. Idea tomada de
   [noeldvictor/Cemu-thor-experiment@3a61e0d](https://github.com/noeldvictor/Cemu-thor-experiment/commit/3a61e0d5dc23ad860efe5d337a447e76c86b0332);
   NO se adoptó la otra parte de ese commit (sustituir `_SyncForPendingQueryResult()` por `GX2DrawDone()`)
   porque `GX2DrawDone` solo emite `IT_HLE_SYNC_ASYNC_OPERATIONS` en Vulkan o con la opción "full sync",
   y en OpenGL/Metal dejaría de garantizar el resultado. Sin PR upstream (solo en `main` del fork).

## Qué cambia y por qué

Archivo: `src/Cafe/OS/libs/gx2/GX2_Query.cpp`

ZombiU (motor LyN de Ubisoft) comprueba si cada luz está tapada dibujando un quad
invisible en su posición dentro de una **occlusion query de GX2 de tipo CPU**, y
lee el resultado con `GX2QueryGetOcclusionResult` **pocos milisegundos después,
en el mismo frame**. Si el resultado es 0 muestras apaga el destello; si hay
muestras, o si el resultado *aún no está listo*, lo dibuja a plena intensidad.

En la consola funciona porque el juego espera a la GPU al final de cada frame
(`GX2WaitTimeStamp`), así que la GPU nunca va retrasada y el resultado ya está
escrito cuando el juego pregunta. En Cemu el hilo de GPU va por detrás del hilo
PPC y el ~94% de las consultas devolvían "no listo" → destello siempre visible.

El fix: en `GX2QueryGetOcclusionResult`, si la query CPU sigue pendiente, se
emite el paquete `IT_HLE_SYNC_ASYNC_OPERATIONS`, se hace flush y se espera a que
la GPU retire el trabajo (lo mismo que hace `GX2DrawDone` con sincronización
completa) y se vuelve a comprobar. Solo paga el coste quien realmente sondea una
query pendiente (~1,6 sincronizaciones/frame en ZombiU, sin pérdida de FPS).

**Descartado durante la investigación** (no repetir): depth bounds test (el juego
no lo usa), render condicional (el juego no lo importa), problemas de ritmo de
presentación/VSync/DWM, y una primera versión del fix que devolvía el resultado
del frame anterior (rompía el motion blur del juego: copias múltiples de los
objetos al mover la cámara, porque otro sistema del juego también consume esas
queries y espera el resultado del frame actual).

## Ramas

- `main`: upstream `main` + el fix + este fichero + `workflow_dispatch` en
  `.github/workflows/build_check.yml` (solo para poder lanzar builds a mano en
  el fork).
- `fix/cpu-occlusion-query-sync`: solo el commit del fix de luces (rama del PR #2047).
- `fix/fsc-merged-dir-order`: solo el commit del fix de la actualización v32 (rama del PR #2048).

## Cómo actualizar el fork con el Cemu más reciente (rebase)

Requisitos: `git` y `gh` (GitHub CLI) autenticado como LuismaSP89. Sin toolchain
local: las builds se hacen en GitHub Actions.

```bash
git clone https://github.com/LuismaSP89/Cemu.git
cd Cemu
git remote add upstream https://github.com/cemu-project/Cemu.git
git fetch upstream

# 1) Ramas de los fixes (para los PR): rebase de cada commit sobre upstream/main
for b in fix/cpu-occlusion-query-sync fix/fsc-merged-dir-order; do
  git checkout $b && git rebase upstream/main && git push --force-with-lease origin $b
done

# 2) main del fork: reconstruir = upstream/main + fix + commits propios del fork
git checkout main
git rebase upstream/main
git push --force-with-lease origin main

# 3) Compilar en Actions (Windows x64) y descargar el ejecutable
gh workflow run "Build check" -R LuismaSP89/Cemu --ref main
gh run list -R LuismaSP89/Cemu --limit 1          # anotar el ID cuando termine (~40 min)
gh run download <RUN_ID> -R LuismaSP89/Cemu -n cemu-bin-windows-x64 -D out
# out/Cemu.exe -> copiar sobre el Cemu.exe de la instalación (comparte settings/mlc01/caches)
```

Si el rebase da conflicto, lo normal es que sea en `GX2_Query.cpp` porque upstream
haya tocado `GX2QueryGetOcclusionResult`: conservar la comprobación de pendiente
+ `_SyncForPendingQueryResult()` + re-comprobación antes de devolver `GX2_FALSE`.
Si upstream ya integró el PR, este fork deja de ser necesario: basta usar la
release oficial.

## Cómo verificar el fix

Cargar ZombiU, ponerse frente a una pared con una lámpara/fluorescente detrás:
el halo no debe verse a través de la pared, ni quieto ni moviéndose, y al
asomarse aparece con un fundido corto. Los FPS deben mantenerse en 30.
