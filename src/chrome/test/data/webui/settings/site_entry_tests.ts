// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
import 'chrome://webui-test/cr_elements/cr_policy_strings.js';

import {loadTimeData} from 'chrome://resources/js/load_time_data.m.js';
import {flush} from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js';
import {LocalDataBrowserProxyImpl, SiteEntryElement, SiteSettingsPrefsBrowserProxyImpl, SortMethod} from 'chrome://settings/lazy_load.js';
import {Router, routes} from 'chrome://settings/settings.js';
import {assertEquals, assertFalse, assertTrue} from 'chrome://webui-test/chai_assert.js';
import {eventToPromise, isChildVisible} from 'chrome://webui-test/test_util.js';

import {TestLocalDataBrowserProxy} from './test_local_data_browser_proxy.js';
import {TestSiteSettingsPrefsBrowserProxy} from './test_site_settings_prefs_browser_proxy.js';
import { createOriginInfo,createSiteGroup} from './test_util.js';

// clang-format on

suite('SiteEntry_DisabledConsolidatedControls', function() {
  /**
   * An example eTLD+1 Object with multiple origins grouped under it.
   */
  const TEST_MULTIPLE_SITE_GROUP = createSiteGroup('example.com', [
    'http://example.com',
    'https://www.example.com',
    'https://login.example.com',
  ]);

  /**
   * An example eTLD+1 Object with a single origin in it.
   */
  const TEST_SINGLE_SITE_GROUP = createSiteGroup('foo.com', [
    'https://login.foo.com',
  ]);

  /**
   * The mock proxy object to use during test.
   */
  let browserProxy: TestSiteSettingsPrefsBrowserProxy;

  /**
   * The mock local data proxy object to use during test.
   */
  let localDataBrowserProxy: TestLocalDataBrowserProxy;

  /**
   * A site list element created before each test.
   */
  let testElement: SiteEntryElement;

  suiteSetup(function() {
    loadTimeData.overrideValues({
      consolidatedSiteStorageControlsEnabled: false,
    });
  });

  // Initialize a site-list before each test.
  setup(function() {
    browserProxy = new TestSiteSettingsPrefsBrowserProxy();
    localDataBrowserProxy = new TestLocalDataBrowserProxy();
    SiteSettingsPrefsBrowserProxyImpl.setInstance(browserProxy);
    LocalDataBrowserProxyImpl.setInstance(localDataBrowserProxy);

    document.body.innerHTML = '';
    testElement = document.createElement('site-entry');
    document.body.appendChild(testElement);
  });

  teardown(function() {
    // The code being tested changes the Route. Reset so that state is not
    // leaked across tests.
    Router.getInstance().resetRouteForTesting();
  });

  test('displays the correct number of origins', function() {
    testElement.siteGroup = TEST_MULTIPLE_SITE_GROUP;
    flush();
    const collapseChild = testElement.$.originList.get();
    flush();
    assertEquals(3, collapseChild.querySelectorAll('.origin-link').length);
  });

  test('expands and closes to show more origins', function() {
    testElement.siteGroup = TEST_MULTIPLE_SITE_GROUP;
    assertFalse(testElement.$.expandIcon.hidden);
    assertEquals(
        'false', testElement.$.toggleButton.getAttribute('aria-expanded'));
    const originList = testElement.$.originList.get();
    assertTrue(originList.classList.contains('iron-collapse-closed'));
    assertEquals('true', originList.getAttribute('aria-hidden'));

    testElement.$.toggleButton.click();
    assertEquals(
        'true', testElement.$.toggleButton.getAttribute('aria-expanded'));
    assertTrue(originList.classList.contains('iron-collapse-opened'));
    assertEquals('false', originList.getAttribute('aria-hidden'));
  });

  test('with single origin navigates to Site Details', function() {
    testElement.siteGroup = TEST_SINGLE_SITE_GROUP;
    assertTrue(testElement.$.expandIcon.hidden);
    assertEquals(
        'false', testElement.$.toggleButton.getAttribute('aria-expanded'));
    const originList = testElement.$.originList.get();
    assertTrue(originList.classList.contains('iron-collapse-closed'));
    assertEquals('true', originList.getAttribute('aria-hidden'));

    testElement.$.toggleButton.click();
    assertEquals(
        'false', testElement.$.toggleButton.getAttribute('aria-expanded'));
    assertTrue(originList.classList.contains('iron-collapse-closed'));
    assertEquals('true', originList.getAttribute('aria-hidden'));
    assertEquals(
        routes.SITE_SETTINGS_SITE_DETAILS.path,
        Router.getInstance().getCurrentRoute().path);
    assertEquals(
        'https://login.foo.com',
        Router.getInstance().getQueryParameters().get('site'));
  });

  test('with multiple origins navigates to Site Details', function() {
    testElement.siteGroup = TEST_MULTIPLE_SITE_GROUP;
    flush();
    const collapseChild = testElement.$.originList.get();
    flush();
    const originList =
        collapseChild.querySelectorAll<HTMLElement>('.origin-link');
    assertEquals(3, originList.length);

    // Test clicking on one of these origins takes the user to Site Details,
    // with the correct origin.
    originList[1]!.click();
    assertEquals(
        routes.SITE_SETTINGS_SITE_DETAILS.path,
        Router.getInstance().getCurrentRoute().path);
    assertEquals(
        TEST_MULTIPLE_SITE_GROUP.origins[1]!.origin,
        Router.getInstance().getQueryParameters().get('site'));
  });

  test('with single origin, shows overflow menu', function() {
    testElement.siteGroup = TEST_SINGLE_SITE_GROUP;
    flush();
    const overflowMenuButton =
        testElement.$$<HTMLElement>('#overflowMenuButton')!;
    assertFalse(
        overflowMenuButton.closest<HTMLElement>('.row-aligned')!.hidden);
  });

  test('clear data for single origin fires the right method', async function() {
    testElement.siteGroup =
        JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    flush();

    const collapseChild = testElement.$.originList.get();
    flush();
    const originList = collapseChild.querySelectorAll('.hr');
    assertEquals(3, originList.length);

    for (let i = 0; i < originList.length; i++) {
      const menuOpened = eventToPromise('open-menu', testElement);
      const originEntry = originList[i]!;
      const overflowMenuButton =
          originEntry.querySelector<HTMLElement>('#originOverflowMenuButton')!;
      overflowMenuButton.click();
      const openMenuEvent = await menuOpened;

      const args = openMenuEvent.detail;
      const {actionScope, index, origin} = args;
      assertEquals('origin', actionScope);
      assertEquals(testElement.listIndex, index);
      assertEquals(testElement.siteGroup.origins[i]!.origin, origin);
    }
  });

  test(
      'moving from grouped to ungrouped does not get stuck in opened state',
      function() {
        // Clone this object to avoid propagating changes made in this test.
        testElement.siteGroup =
            JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
        flush();
        testElement.$.toggleButton.click();
        assertTrue(testElement.$.originList.get().opened);

        // Remove all origins except one, then make sure it's not still
        // expanded.
        const siteGroupUpdated =
            JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
        siteGroupUpdated.origins.splice(1);
        testElement.siteGroup = siteGroupUpdated;
        assertEquals(1, testElement.siteGroup.origins.length);
        assertFalse(testElement.$.originList.get().opened);
      });

  test('cookies show correctly for grouped entries', async function() {
    testElement.siteGroup = TEST_MULTIPLE_SITE_GROUP;
    flush();
    const cookiesLabel = testElement.$.cookies;
    assertTrue(cookiesLabel.hidden);
    // When the number of cookies is more than zero, the label appears.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    const numCookies = 3;
    testSiteGroup.numCookies = numCookies;

    testElement.siteGroup = testSiteGroup;

    flush();
    const args = await localDataBrowserProxy.whenCalled('getNumCookiesString');
    assertEquals(3, args);
    assertFalse(cookiesLabel.hidden);
    assertEquals('· 3 cookies', cookiesLabel.textContent!.trim());
  });

  test('cookies show for ungrouped entries', async function() {
    testElement.siteGroup = TEST_SINGLE_SITE_GROUP;
    flush();
    const cookiesLabel = testElement.$.cookies;
    assertTrue(cookiesLabel.hidden);


    const testSiteGroup = JSON.parse(JSON.stringify(TEST_SINGLE_SITE_GROUP));
    const numCookies = 3;

    testSiteGroup.numCookies = numCookies;

    testElement.siteGroup = testSiteGroup;

    flush();
    const args = await localDataBrowserProxy.whenCalled('getNumCookiesString');
    assertEquals(3, args);
    assertFalse(cookiesLabel.hidden);
    assertEquals('· 3 cookies', cookiesLabel.textContent!.trim());
  });

  test('data usage shown correctly for grouped entries', async function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    const numBytes1 = 74622;
    const numBytes2 = 1274;
    const numBytes3 = 0;
    testSiteGroup.origins[0].usage = numBytes1;
    testSiteGroup.origins[1].usage = numBytes2;
    testSiteGroup.origins[2].usage = numBytes3;
    testElement.siteGroup = testSiteGroup;
    flush();
    const args = await browserProxy.whenCalled('getFormattedBytes');
    const sumBytes = numBytes1 + numBytes2 + numBytes3;
    assertEquals(sumBytes, args);
    assertEquals(
        `${sumBytes} B`,
        testElement.shadowRoot!
            .querySelector<HTMLElement>(
                '#displayName .data-unit')!.textContent!.trim());
  });

  test('data usage shown correctly for ungrouped entries', async function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_SINGLE_SITE_GROUP));
    const numBytes = 74622;
    testSiteGroup.origins[0].usage = numBytes;
    testElement.siteGroup = testSiteGroup;
    flush();
    const args = await browserProxy.whenCalled('getFormattedBytes');
    assertEquals(numBytes, args);
    assertEquals(
        `${numBytes} B`,
        testElement.shadowRoot!
            .querySelector<HTMLElement>(
                '#displayName .data-unit')!.textContent!.trim());
  });

  test(
      'large number data usage shown correctly for grouped entries',
      async function() {
        // Clone this object to avoid propagating changes made in this test.
        const testSiteGroup =
            JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
        const numBytes1 = 2000000000;
        const numBytes2 = 10000000000;
        const numBytes3 = 7856;
        testSiteGroup.origins[0].usage = numBytes1;
        testSiteGroup.origins[1].usage = numBytes2;
        testSiteGroup.origins[2].usage = numBytes3;
        testElement.siteGroup = testSiteGroup;
        flush();
        const args = await browserProxy.whenCalled('getFormattedBytes');
        const sumBytes = numBytes1 + numBytes2 + numBytes3;
        assertEquals(sumBytes, args);
        assertEquals(
            `${sumBytes} B`,
            testElement.shadowRoot!
                .querySelector<HTMLElement>(
                    '#displayName .data-unit')!.textContent!.trim());
      });

  test('favicon with www.etld+1 chosen for site group', function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    testSiteGroup.origins[0].usage = 0;
    testSiteGroup.origins[1].usage = 1274;
    testSiteGroup.origins[2].usage = 74622;
    testElement.siteGroup = testSiteGroup;
    flush();
    assertEquals(
        testElement.$.collapseParent.querySelector('site-favicon')!.url,
        'https://www.example.com');
  });

  test('favicon with largest storage chosen for site group', function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    testSiteGroup.origins[0].usage = 0;
    testSiteGroup.origins[1].usage = 1274;
    testSiteGroup.origins[2].usage = 74622;
    testSiteGroup.origins[1].origin = 'https://abc.example.com';
    testElement.siteGroup = testSiteGroup;
    flush();
    assertEquals(
        testElement.$.collapseParent.querySelector('site-favicon')!.url,
        'https://login.example.com');
  });

  test('favicon with largest cookies number chosen for site group', function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    testSiteGroup.origins[0].usage = 0;
    testSiteGroup.origins[1].usage = 1274;
    testSiteGroup.origins[2].usage = 1274;
    testSiteGroup.origins[0].numCookies = 10;
    testSiteGroup.origins[1].numCookies = 3;
    testSiteGroup.origins[2].numCookies = 1;
    testSiteGroup.origins[1].origin = 'https://abc.example.com';
    testElement.siteGroup = testSiteGroup;
    flush();
    assertEquals(
        testElement.$.collapseParent.querySelector('site-favicon')!.url,
        'https://abc.example.com');
  });

  test('can be sorted by most visited', function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    testSiteGroup.origins[0].engagement = 20;
    testSiteGroup.origins[1].engagement = 30;
    testSiteGroup.origins[2].engagement = 10;
    testSiteGroup.origins[0].usage = 0;
    testSiteGroup.origins[1].usage = 1274;
    testSiteGroup.origins[2].usage = 1274;
    testSiteGroup.origins[0].numCookies = 10;
    testSiteGroup.origins[1].numCookies = 3;
    testSiteGroup.origins[2].numCookies = 1;
    testElement.sortMethod = SortMethod.MOST_VISITED;
    testElement.siteGroup = testSiteGroup;
    flush();
    const collapseChild = testElement.$.originList.get();
    flush();
    const origins = collapseChild.querySelectorAll('.origin-link');
    assertEquals(3, origins.length);
    assertEquals(
        'www.example.com',
        origins[0]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
    assertEquals(
        'example.com',
        origins[1]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
    assertEquals(
        'login.example.com',
        origins[2]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
  });

  test('can be sorted by storage', function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    testSiteGroup.origins[0].engagement = 20;
    testSiteGroup.origins[1].engagement = 30;
    testSiteGroup.origins[2].engagement = 10;
    testSiteGroup.origins[0].usage = 0;
    testSiteGroup.origins[1].usage = 1274;
    testSiteGroup.origins[2].usage = 1274;
    testSiteGroup.origins[0].numCookies = 10;
    testSiteGroup.origins[1].numCookies = 3;
    testSiteGroup.origins[2].numCookies = 1;
    testElement.sortMethod = SortMethod.STORAGE;
    testElement.siteGroup = testSiteGroup;
    flush();
    const collapseChild = testElement.$.originList.get();
    flush();
    const origins = collapseChild.querySelectorAll('.origin-link');
    assertEquals(3, origins.length);
    assertEquals(
        'www.example.com',
        origins[0]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
    assertEquals(
        'login.example.com',
        origins[1]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
    assertEquals(
        'example.com',
        origins[2]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
  });

  test('can be sorted by name', function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    testSiteGroup.origins[0].engagement = 20;
    testSiteGroup.origins[1].engagement = 30;
    testSiteGroup.origins[2].engagement = 10;
    testSiteGroup.origins[0].usage = 0;
    testSiteGroup.origins[1].usage = 1274;
    testSiteGroup.origins[2].usage = 1274;
    testSiteGroup.origins[0].numCookies = 10;
    testSiteGroup.origins[1].numCookies = 3;
    testSiteGroup.origins[2].numCookies = 1;
    testElement.sortMethod = SortMethod.NAME;
    testElement.siteGroup = testSiteGroup;
    flush();
    const collapseChild = testElement.$.originList.get();
    flush();
    const origins = collapseChild.querySelectorAll('.origin-link');
    assertEquals(3, origins.length);
    assertEquals(
        'example.com',
        origins[0]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
    assertEquals(
        'login.example.com',
        origins[1]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
    assertEquals(
        'www.example.com',
        origins[2]!.querySelector<HTMLElement>(
                       '#originSiteRepresentation')!.innerText.trim());
  });
});

suite('SiteEntry_EnabledConsolidatedControls', function() {
  /**
   * An example eTLD+1 Object with multiple origins grouped under it.
   */
  const TEST_MULTIPLE_SITE_GROUP = createSiteGroup('example.com', [
    'http://example.com',
    'https://www.example.com',
    'https://login.example.com',
  ]);
  /**
   * An example eTLD+1 Object with a single origin in it.
   */
  const TEST_SINGLE_SITE_GROUP = createSiteGroup('foo.com', [
    'https://login.foo.com',
  ]);

  /**
   * The mock proxy object to use during test.
   */
  let browserProxy: TestSiteSettingsPrefsBrowserProxy;

  /**
   * The mock local data proxy object to use during test.
   */
  let localDataBrowserProxy: TestLocalDataBrowserProxy;

  /**
   * A site list element created before each test.
   */
  let testElement: SiteEntryElement;

  suiteSetup(function() {
    loadTimeData.overrideValues({
      consolidatedSiteStorageControlsEnabled: true,
    });
  });

  // Initialize a site-list before each test.
  setup(function() {
    browserProxy = new TestSiteSettingsPrefsBrowserProxy();
    localDataBrowserProxy = new TestLocalDataBrowserProxy();
    SiteSettingsPrefsBrowserProxyImpl.setInstance(browserProxy);
    LocalDataBrowserProxyImpl.setInstance(localDataBrowserProxy);

    document.body.innerHTML = '';
    testElement = document.createElement('site-entry');
    document.body.appendChild(testElement);
  });

  test('remove site fires correct event for individual site', async function() {
    testElement.siteGroup =
        JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    flush();

    const collapseChild = testElement.$.originList.get();
    flush();
    const originList = collapseChild.querySelectorAll('.hr');
    assertEquals(3, originList.length);

    for (let i = 0; i < originList.length; i++) {
      const siteRemoved = eventToPromise('remove-site', testElement);
      originList[i]!.querySelector<HTMLElement>('#removeOriginButton')!.click();
      const siteRemovedEvent = await siteRemoved;

      const {actionScope, index, origin} = siteRemovedEvent.detail;
      assertEquals('origin', actionScope);
      assertEquals(testElement.listIndex, index);
      assertEquals(testElement.siteGroup.origins[i]!.origin, origin);
    }
  });

  test('remove site fires correct event for site group', async function() {
    testElement.siteGroup =
        JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));
    flush();

    const siteRemoved = eventToPromise('remove-site', testElement);
    testElement.$$<HTMLElement>('#removeSiteButton')!.click();
    const siteRemovedEvent = await siteRemoved;

    const {actionScope, index, origin} = siteRemovedEvent.detail;

    // Site groups are removed exclusively based on their index.
    assertEquals(undefined, actionScope);
    assertEquals(testElement.listIndex, index);
    assertEquals(undefined, origin);
  });

  test('partitioned entry interaction', async function() {
    // Clone this object to avoid propagating changes made in this test.
    const testSiteGroup = JSON.parse(JSON.stringify(TEST_MULTIPLE_SITE_GROUP));

    // Add a partitioned entry for an unrelated origin.
    testSiteGroup.origins.push(
        createOriginInfo('wwww.unrelated.com', {isPartitioned: true}));

    testElement.siteGroup = testSiteGroup;
    flush();
    const collapseChild = testElement.$.originList.get();
    testElement.$.toggleButton.click();
    flush();

    const originList = collapseChild.querySelectorAll('.hr');
    assertEquals(4, originList.length);

    // Partitioned entries should not be displaying a link arrow, while
    // unpartitioned entries should.
    assertTrue(isChildVisible(
        originList[0]!, 'cr-icon-button', /*checkLightDom=*/ true));
    assertFalse(isChildVisible(
        originList[3]!, 'cr-icon-button', /*checkLightDom=*/ true));

    // Removing a partitioned entry should fire the appropriate event.
    const siteRemoved = eventToPromise('remove-site', testElement);
    originList[3]!.querySelector<HTMLElement>('#removeOriginButton')!.click();
    const siteRemovedEvent = await siteRemoved;

    const args = siteRemovedEvent.detail;
    const {actionScope, index, origin, isPartitioned} = args;
    assertEquals('origin', actionScope);
    assertEquals(testElement.listIndex, index);
    assertEquals(testElement.siteGroup.origins[3]!.origin, origin);
    assertTrue(isPartitioned);
  });

  test('partitioned entry prevents collapse', function() {
    // If a siteGroup has a partitioned entry, even if it is the only entry,
    // it should keep the site entry as a top level + collapse list.
    const testSingleSite = JSON.parse(JSON.stringify(TEST_SINGLE_SITE_GROUP));
    testSingleSite.origins[0].isPartitioned = true;

    testElement.siteGroup = testSingleSite;
    flush();
    const collapseChild = testElement.$.originList.get();

    // The toggle button should expand the collapse, rather than navigate.
    const startingRoute = Router.getInstance().getCurrentRoute();
    testElement.$.toggleButton.click();
    flush();
    assertEquals(startingRoute, Router.getInstance().getCurrentRoute());

    const originList = collapseChild.querySelectorAll('.hr');
    assertEquals(1, originList.length);
  });

  test('unpartitioned entry remains collapsed', async function() {
    // Check that a single origin containing unpartitioned storage only is
    // correctly collapsed.
    testElement.siteGroup = JSON.parse(JSON.stringify(TEST_SINGLE_SITE_GROUP));
    flush();
    const collapseChild = testElement.$.originList.get();

    const originList = collapseChild.querySelectorAll('.hr');
    testElement.$.toggleButton.click();
    flush();
    assertEquals(0, originList.length);

    // Clicking the toggleButton should navigate the page away, as there is
    // only one entry.
    testElement.$.toggleButton.click();
    flush();
    assertEquals(
        routes.SITE_SETTINGS_SITE_DETAILS.path,
        Router.getInstance().getCurrentRoute().path);
  });
});
