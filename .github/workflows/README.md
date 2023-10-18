# 23.3 GitHub CI/CD Pipeline

Comparison between ClickHouse and Altinity CI/CD pipeline. Primary focus on the comparison is on testing and builds.  
ClickHouse workflows from https://github.com/ClickHouse/ClickHouse/tree/ff5101070e4732977e72d810f0e0ca1042dd4d3a.  

## Master only

The following workflow files are included but they're only run on the master branch:
- jepsen.yml
- nightly.yml
- master.yml
- woboq.yml

## Not included workflows

The following workflow files are not included in the comparison as they are minor, not used, or do not provide any new tests relative to other workflows:
- backport_branches.yml
- cancel.yml
- cherry_pick.yml
- debug.yml
- pull_request_approved.yml
- release.yml
- tags_stable.yml
- docs_check.yml

## pull_request.yml

https://github.com/ClickHouse/ClickHouse/blob/23.3/.github/workflows/pull_request.yml

| Job  | In altinity pipeline? |  
| ------ | ------------ |  
| CheckLabels | no, ClickHouse PR label management tool. | 
| PythonUnitTests | no, ClickHouse infrastructure tests. | 
| DockerHubPushAarch64 | yes | 
| DockerHubPushAmd64 | yes | 
| DockerHubPush | yes | 
| StyleCheck | yes | 
| FastTest | yes | 
| CompatibilityCheckX86 | yes | 
| CompatibilityCheckAarch64 | yes | 
| BuilderDebRelease | yes | 
| BuilderBinRelease | no, we are focused on deb. | 
| BuilderDebAarch64 | yes | 
| BuilderDebAsan | yes |
| BuilderDebUBsan | yes | 
| BuilderDebTsan | yes | 
| BuilderDebMsan | yes |
| BuilderDebDebug | yes | 
| BuilderBinClangTidy | no, we are focused on deb. | 
| BuilderBinDarwin | no, we are focused on deb. | 
| BuilderBinAarch64 | no, we are focused on deb. | 
| BuilderBinFreeBSD | no, we are focused on deb. | 
| BuilderBinDarwinAarch64 | no, we are focused on deb. | 
| BuilderBinPPC64| no, we are focused on deb. | 
| BuilderBinAmd64Compat | no, we are focused on deb. | 
| BuilderBinAarch64V80Compat | no, we are focused on deb. | 
| DockerServerImages | yes | 
| BuilderReport | yes | 
| BuilderSpecialReport | no, depends on bin builder stages. | 
| InstallPackagesTestRelease | yes | 
| InstallPackagesTestAarch64 | yes | 
| FunctionalStatelessTestRelease | yes | 
| FunctionalStatelessTestReleaseDatabaseReplicated0 | yes | 
| FunctionalStatelessTestReleaseDatabaseReplicated1 | yes | 
| FunctionalStatelessTestReleaseDatabaseReplicated2 | yes | 
| FunctionalStatelessTestReleaseDatabaseReplicated3 | yes | 
| FunctionalStatelessTestReleaseWideParts | yes | 
| FunctionalStatelessTestReleaseS3_0 | yes | 
| FunctionalStatelessTestReleaseS3_1 | yes | 
| FunctionalStatelessTestS3Debug0 | yes | 
| FunctionalStatelessTestS3Debug1 | yes | 
| FunctionalStatelessTestS3Debug2 | yes | 
| FunctionalStatelessTestS3Debug3 | yes | 
| FunctionalStatelessTestS3Debug4 | yes | 
| FunctionalStatelessTestS3Debug5 | yes | 
| FunctionalStatelessTestS3Tsan0 | yes | 
| FunctionalStatelessTestS3Tsan1 | yes | 
| FunctionalStatelessTestS3Tsan2 | yes | 
| FunctionalStatelessTestS3Tsan3 | yes | 
| FunctionalStatelessTestS3Tsan4 | yes | 
| FunctionalStatelessTestAarch64 | yes | 
| FunctionalStatelessTestAsan0 | yes | 
| FunctionalStatelessTestAsan1 | yes | 
| FunctionalStatelessTestAsan2 | yes | 
| FunctionalStatelessTestAsan3 | yes | 
| FunctionalStatelessTestTsan0 | yes | 
| FunctionalStatelessTestTsan1 | yes | 
| FunctionalStatelessTestTsan2 | yes | 
| FunctionalStatelessTestTsan3 | yes | 
| FunctionalStatelessTestTsan4 | yes | 
| FunctionalStatelessTestUBsan0 | yes | 
| FunctionalStatelessTestUBsan1 | yes | 
| FunctionalStatelessTestMsan0 | yes | 
| FunctionalStatelessTestMsan1 | yes | 
| FunctionalStatelessTestMsan2 | yes | 
| FunctionalStatelessTestMsan3 | yes | 
| FunctionalStatelessTestMsan4 | yes | 
| FunctionalStatelessTestMsan5 | yes | 
| FunctionalStatelessTestDebug0 | yes | 
| FunctionalStatelessTestDebug1 | yes | 
| FunctionalStatelessTestDebug2 | yes | 
| FunctionalStatelessTestDebug3 | yes | 
| FunctionalStatelessTestDebug4 | yes | 
| FunctionalStatelessTestFlakyCheck | yes | 
| TestsBugfixCheck | no, depends on CheckLabels | 
| FunctionalStatefulTestRelease | yes | 
| FunctionalStatefulTestAarch64 | yes | 
| FunctionalStatefulTestAsan | yes | 
| FunctionalStatefulTestTsan | yes | 
| FunctionalStatefulTestMsan | yes | 
| FunctionalStatefulTestUBsan | yes | 
| FunctionalStatefulTestDebug | yes | 
| FunctionalStatefulTestDebugParallelReplicas | yes | 
| FunctionalStatefulTestUBsanParallelReplicas | yes | 
| FunctionalStatefulTestMsanParallelReplicas | yes | 
| FunctionalStatefulTestTsanParallelReplicas | yes | 
| FunctionalStatefulTestAsanParallelReplicas | yes | 
| FunctionalStatefulTestReleaseParallelReplicas | yes | 
| StressTestAsan | yes | 
| StressTestTsan | yes | 
| StressTestMsan | yes | 
| StressTestUBsan | yes | 
| StressTestDebug | yes | 
| UpgradeCheckAsan | yes | 
| UpgradeCheckTsan | yes | 
| UpgradeCheckMsan | yes | 
| UpgradeCheckDebug | yes | 
| ASTFuzzerTestAsan | yes | 
| ASTFuzzerTestTsan | yes | 
| ASTFuzzerTestUBSan | yes | 
| ASTFuzzerTestMSan | yes | 
| ASTFuzzerTestDebug | yes | 
| IntegrationTestsAsan0 | yes | 
| IntegrationTestsAsan1 | yes | 
| IntegrationTestsAsan2 | yes | 
| IntegrationTestsAsan3 | yes | 
| IntegrationTestsAsan4 | yes | 
| IntegrationTestsAsan5 | yes | 
| IntegrationTestsTsan0 | yes | 
| IntegrationTestsTsan1 | yes | 
| IntegrationTestsTsan2 | yes | 
| IntegrationTestsTsan3 | yes | 
| IntegrationTestsTsan4 | yes | 
| IntegrationTestsTsan5 | yes | 
| IntegrationTestsRelease0 | yes | 
| IntegrationTestsRelease1 | yes | 
| IntegrationTestsRelease2 | yes | 
| IntegrationTestsRelease3 | yes | 
| IntegrationTestsFlakyCheck | yes | 
| UnitTestsAsan | yes | 
| UnitTestsReleaseClang | no, depends on BuilderBinRelease. | 
| UnitTestsTsan | yes | 
| UnitTestsMsan | yes | 
| UnitTestsUBsan | yes | 
| PerformanceComparisonX86-0 | no, to be added. | 
| PerformanceComparisonX86-1 | no, to be added. | 
| PerformanceComparisonX86-2 | no, to be added. | 
| PerformanceComparisonX86-3 | no, to be added. | 
| PerformanceComparisonAarch-0 | no, to be added. | 
| PerformanceComparisonAarch-1 | no, to be added. | 
| PerformanceComparisonAarch-2 | no, to be added. | 
| PerformanceComparisonAarch-3 | no, to be added. | 
| SQLancerTestRelease | yes | 
| SQLancerTestDebug | yes | 
| Jepsen* | no, depends on BuilderBinRelease. | 

Jepsen job only runs if the pull request name containes `jepsen-test`

## release_branches.yml  

https://github.com/ClickHouse/ClickHouse/blob/23.3/.github/workflows/release_branches.yml

| Job  | In altinity pipeline? |  
| ------ | ------------ |  
| DockerHubPushAarch64 | yes |
| DockerHubPushAmd64 | yes |
| DockerHubPush | yes | 
| CompatibilityCheckX86 | yes | 
| CompatibilityCheckAarch64 | yes | 
| BuilderDebRelease | yes | 
| BuilderDebAarch64 | yes | 
| BuilderDebAsan | yes |
| BuilderDebUBsan | yes | 
| BuilderDebTsan | yes | 
| BuilderDebMsan | yes |
| BuilderDebDebug | yes | 
| BuilderBinDarwin | no, we are only focused on deb. | 
| BuilderBinDarwinAarch64 | no, we are only focused on deb. | 
| DockerServerImages | yes | 
| BuilderReport | yes | 
| BuilderSpecialReport | no, depends on bin builder stages. | 
| MarkReleaseReady | yes | 
| InstallPackagesTestRelease | yes | 
| InstallPackagesTestAarch64 | yes | 
| FunctionalStatelessTestRelease | yes | 
| FunctionalStatelessTestAarch64 | yes | 
| FunctionalStatelessTestAsan0 | yes | 
| FunctionalStatelessTestAsan1 | yes | 
| FunctionalStatelessTestTsan0 | yes | 
| FunctionalStatelessTestTsan1 | yes | 
| FunctionalStatelessTestTsan2 | yes | 
| FunctionalStatelessTestUBsan | yes | 
| FunctionalStatelessTestMsan0 | yes | 
| FunctionalStatelessTestMsan1 | yes | 
| FunctionalStatelessTestMsan2 | yes | 
| FunctionalStatelessTestDebug0 | yes | 
| FunctionalStatelessTestDebug1 | yes | 
| FunctionalStatelessTestDebug2 | yes | 
| FunctionalStatefulTestRelease | yes | 
| FunctionalStatefulTestAarch64 | yes | 
| FunctionalStatefulTestAsan | yes | 
| FunctionalStatefulTestTsan | yes | 
| FunctionalStatefulTestMsan | yes | 
| FunctionalStatefulTestUBsan | yes | 
| FunctionalStatefulTestDebug | yes |
| StressTestAsan | yes | 
| StressTestTsan | yes | 
| StressTestMsan | yes | 
| StressTestUBsan | yes | 
| StressTestDebug | yes | 
| IntegrationTestsAsan0 | yes | 
| IntegrationTestsAsan1 | yes | 
| IntegrationTestsAsan2 | yes | 
| IntegrationTestsTsan0 | yes | 
| IntegrationTestsTsan1 | yes | 
| IntegrationTestsTsan2 | yes | 
| IntegrationTestsTsan3 | yes | 
| IntegrationTestsRelease0 | yes | 
| IntegrationTestsRelease1 | yes | 
| FinishCheck | yes | 
