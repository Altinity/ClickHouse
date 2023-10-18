# 23.3 GitHub CI/CD Pipeline

Comparison between ClickHouse and Altinity CI/CD pipeline. Primary focus on the comparison is on testing and builds. 
ClickHouse jobs from commit ff1432de0389ae57732f73f2189216e2f7fe0e0f in branch 23.3.  

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

## docs_check.yml

Not run on 23.3, but runs on branches.

https://github.com/ClickHouse/ClickHouse/blob/23.3/.github/workflows/docs_check.yml

| Job  | In altinity pipeline? |  
| ------ | ------------ |  
| CheckLabels | | 
| DockerHubPushAarch64 | yes | 
| DockerHubPushAmd64 | yes | 
| DockerHubPush | yes | 
| StyleCheck | | 
| DocsCheck | | 
| FinishCheck | | 

## pull_request.yml

https://github.com/ClickHouse/ClickHouse/blob/23.3/.github/workflows/pull_request.yml

| Job  | In altinity pipeline? |  
| ------ | ------------ |  
| CheckLabels |  | 
| PythonUnitTests |  | 
| DockerHubPushAarch64 | yes | 
| DockerHubPushAmd64 | yes | 
| DockerHubPush | yes | 
| StyleCheck |  | 
| FastTest |  | 
| CompatibilityCheckX86 | yes | 
| CompatibilityCheckAarch64 | yes | 
| BuilderDebRelease | yes | 
| BuilderBinRelease |  | 
| BuilderDebAarch64 | yes | 
| BuilderDebAsan | yes |
| BuilderDebUBsan | yes | 
| BuilderDebTsan | yes | 
| BuilderDebMsan | yes |
| BuilderDebDebug | yes | 
| BuilderBinClangTidy |  | 
| BuilderBinDarwin |  | 
| BuilderBinAarch64 | | 
| BuilderBinFreeBSD |  | 
| BuilderBinDarwinAarch64 |  | 
| BuilderBinPPC64|  | 
| BuilderBinAmd64Compat |  | 
| BuilderBinAarch64V80Compat |  | 
| DockerServerImages | yes | 
| BuilderReport | yes | 
| BuilderSpecialReport |  | 
| InstallPackagesTestRelease | yes | 
| InstallPackagesTestAarch64 | yes | 
| FunctionalStatelessTestRelease | yes | 
| FunctionalStatelessTestReleaseDatabaseReplicated0 |  | 
| FunctionalStatelessTestReleaseDatabaseReplicated1 |  | 
| FunctionalStatelessTestReleaseDatabaseReplicated2 |  | 
| FunctionalStatelessTestReleaseDatabaseReplicated3 |  | 
| FunctionalStatelessTestReleaseWideParts |  | 
| FunctionalStatelessTestReleaseS3_0 |  | 
| FunctionalStatelessTestReleaseS3_1 |  | 
| FunctionalStatelessTestS3Debug0 |  | 
| FunctionalStatelessTestS3Debug1 |  | 
| FunctionalStatelessTestS3Debug2 |  | 
| FunctionalStatelessTestS3Debug3 |  | 
| FunctionalStatelessTestS3Debug4 |  | 
| FunctionalStatelessTestS3Debug5 |  | 
| FunctionalStatelessTestS3Tsan0 |  | 
| FunctionalStatelessTestS3Tsan1 |  | 
| FunctionalStatelessTestS3Tsan2 |  | 
| FunctionalStatelessTestS3Tsan3 |  | 
| FunctionalStatelessTestS3Tsan4 |  | 
| FunctionalStatelessTestAarch64 | | 
| FunctionalStatelessTestAsan0 | yes | 
| FunctionalStatelessTestAsan1 | yes | 
| FunctionalStatelessTestAsan2 |  | 
| FunctionalStatelessTestAsan3 |  | 
| FunctionalStatelessTestTsan0 | yes | 
| FunctionalStatelessTestTsan1 | yes | 
| FunctionalStatelessTestTsan2 | yes | 
| FunctionalStatelessTestTsan3 |  | 
| FunctionalStatelessTestTsan4 |  | 
| FunctionalStatelessTestUBsan0 | yes | 
| FunctionalStatelessTestUBsan1 |  | 
| FunctionalStatelessTestMsan0 | yes | 
| FunctionalStatelessTestMsan1 | yes | 
| FunctionalStatelessTestMsan2 | yes | 
| FunctionalStatelessTestMsan3 |  | 
| FunctionalStatelessTestMsan4 |  | 
| FunctionalStatelessTestMsan5 |  | 
| FunctionalStatelessTestDebug0 | yes | 
| FunctionalStatelessTestDebug1 | yes | 
| FunctionalStatelessTestDebug2 | yes | 
| FunctionalStatelessTestDebug3 |  | 
| FunctionalStatelessTestDebug4 |  | 
| FunctionalStatelessTestFlakyCheck |  | 
| TestsBugfixCheck |  | 
| FunctionalStatefulTestRelease | yes | 
| FunctionalStatefulTestAarch64 | yes | 
| FunctionalStatefulTestAsan | yes | 
| FunctionalStatefulTestTsan | yes | 
| FunctionalStatefulTestMsan | yes | 
| FunctionalStatefulTestUBsan | yes | 
| FunctionalStatefulTestDebug | yes | 
| FunctionalStatefulTestDebugParallelReplicas |  | 
| FunctionalStatefulTestUBsanParallelReplicas |  | 
| FunctionalStatefulTestMsanParallelReplicas |  | 
| FunctionalStatefulTestTsanParallelReplicas |  | 
| FunctionalStatefulTestAsanParallelReplicas |  | 
| FunctionalStatefulTestReleaseParallelReplicas |  | 
| StressTestAsan | yes | 
| StressTestTsan | yes | 
| StressTestMsan | yes | 
| StressTestUBsan | yes | 
| StressTestDebug | yes | 
| UpgradeCheckAsan |  | 
| UpgradeCheckTsan |  | 
| UpgradeCheckMsan |  | 
| UpgradeCheckDebug |  | 
| ASTFuzzerTestAsan | yes | 
| ASTFuzzerTestTsan | yes | 
| ASTFuzzerTestUBSan | yes | 
| ASTFuzzerTestMSan | yes | 
| ASTFuzzerTestDebug | yes | 
| IntegrationTestsAsan0 | yes | 
| IntegrationTestsAsan1 | yes | 
| IntegrationTestsAsan2 | yes | 
| IntegrationTestsAsan3 |  | 
| IntegrationTestsAsan4 |  | 
| IntegrationTestsAsan5 |  | 
| IntegrationTestsTsan0 | yes | 
| IntegrationTestsTsan1 | yes | 
| IntegrationTestsTsan2 | yes | 
| IntegrationTestsTsan3 | yes | 
| IntegrationTestsTsan4 |  | 
| IntegrationTestsTsan5 |  | 
| IntegrationTestsRelease0 | yes | 
| IntegrationTestsRelease1 | yes | 
| IntegrationTestsRelease2 |  | 
| IntegrationTestsRelease3 |  | 
| IntegrationTestsFlakyCheck |  | 
| UnitTestsAsan |  | 
| UnitTestsReleaseClang |  | 
| UnitTestsTsan |  | 
| UnitTestsMsan |  | 
| UnitTestsUBsan |  | 
| PerformanceComparisonX86-0 |  | 
| PerformanceComparisonX86-1 |  | 
| PerformanceComparisonX86-2 |  | 
| PerformanceComparisonX86-3 |  | 
| PerformanceComparisonAarch-0 |  | 
| PerformanceComparisonAarch-1 |  | 
| PerformanceComparisonAarch-2 |  | 
| PerformanceComparisonAarch-3 |  | 
| SQLancerTestRelease |  | 
| SQLancerTestDebug |  | 
| Jepsen* |  | 

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
| BuilderBinDarwin |  | 
| BuilderBinDarwinAarch64 |  | 
| DockerServerImages | yes | 
| BuilderReport | yes | 
| BuilderSpecialReport | | 
| MarkReleaseReady | yes | 
| InstallPackagesTestRelease | yes | 
| InstallPackagesTestAarch64 | yes | 
| FunctionalStatelessTestRelease | yes | 
| FunctionalStatelessTestAarch64 | | 
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
